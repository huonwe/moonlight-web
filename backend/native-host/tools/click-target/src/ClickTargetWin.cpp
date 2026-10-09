/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

// Windows: D3D12, a flip-model swap chain, tearing allowed — what a game made
// today presents with. Nothing is drawn but cleared rectangles (the
// background, a bar that moves so every frame differs, the flag's three
// bands): no shader, no pipeline state, nothing a driver can hang on.
//
// When the picture reached the screen comes from the swap chain's own frame
// statistics: the present's number, matched against the last one the OS says
// it showed, and the vblank it showed it at (SyncQPCTime). Whether a present
// went through DWM or skipped it (independent flip, MPO) the API does not say;
// PresentMon, run beside it, does.

#include "ClickTarget.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace clicktarget {
namespace {

constexpr UINT kBuffers = 3;
constexpr const wchar_t* kClass = L"MoonlightWebClickTarget";

int64_t qpcToSteadyUs(int64_t qpc)
{
    static const int64_t frequency = [] {
        LARGE_INTEGER f = {};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return (qpc / frequency) * 1000000 + ((qpc % frequency) * 1000000) / frequency;
}

std::string narrow(const wchar_t* w)
{
    std::string s;
    for (; *w; ++w)
        s += *w < 0x80 ? static_cast<char>(*w) : '?';
    return s;
}

/// @p s as the inside of a JSON string: a GDI name is \\.\DISPLAY5.
std::string jsonText(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

struct Monitor
{
    RECT rect = {};
    std::wstring device;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM out)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        Monitor m;
        m.rect = mi.rcMonitor;
        m.device = mi.szDevice;
        // The primary first, as the OS's own list does not promise.
        auto* list = reinterpret_cast<std::vector<Monitor>*>(out);
        if (mi.dwFlags & MONITORINFOF_PRIMARY)
            list->insert(list->begin(), m);
        else
            list->push_back(m);
    }
    return TRUE;
}

/// The click that has not yet been seen on the screen.
struct PendingClick
{
    int id = 0;
    int64_t downUs = 0;
    int64_t renderUs = 0;
    int64_t presentCallUs = 0;
    int64_t presentUs = 0;
    UINT presentId = 0;
    bool presented = false;
};

struct State
{
    HWND hwnd = nullptr;
    bool quit = false;
    int clicks = 0;
    std::deque<PendingClick> pending;
    int64_t flagUntilUs = 0;
    bool redraw = false;
    bool drawFlag = true;
};

State* g_State = nullptr;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_State && !g_State->drawFlag) {
            ++g_State->clicks;
        } else if (g_State) {
            PendingClick c;
            c.id = ++g_State->clicks;
            c.downUs = steadyNowUs();
            g_State->pending.push_back(c);
            g_State->flagUntilUs = c.downUs + int64_t(kFlagMs) * 1000;
            g_State->redraw = true;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_State) g_State->quit = true;
        return 0;
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    case WM_CLOSE:
    case WM_DESTROY:
        if (g_State) g_State->quit = true;
        return 0;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

class Renderer
{
public:
    std::string error;
    std::string adapterName;
    bool tearing = false;

    bool init(HWND hwnd, const Options& o, const std::wstring& monitorDevice, int width, int height)
    {
        m_Width = width;
        m_Height = height;
        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return fail("no DXGI 1.6");
        ComPtr<IDXGIAdapter1> adapter = pickAdapter(factory.Get(), o.adapter, monitorDevice);
        if (!adapter) return fail("no adapter matches \"" + o.adapter + "\"");
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        adapterName = narrow(desc.Description);
        if (FAILED(
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device))))
            return fail("D3D12CreateDevice failed on " + adapterName);

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_Queue))))
            return fail("no command queue");

        BOOL allowTearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                     sizeof(allowTearing));
        tearing = o.tearing && o.syncInterval == 0 && allowTearing;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = static_cast<UINT>(width);
        sd.Height = static_cast<UINT>(height);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                   (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        m_SwapFlags = sd.Flags;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(m_Queue.Get(), hwnd, &sd, nullptr, nullptr,
                                                   &sc1)) ||
            FAILED(sc1.As(&m_Swap)))
            return fail("no flip-model swap chain");
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        // One frame queued at most: what a game tuned for latency asks.
        m_Swap->SetMaximumFrameLatency(1);
        m_Waitable = m_Swap->GetFrameLatencyWaitableObject();

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBuffers;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_Rtvs))))
            return fail("no RTV heap");
        m_RtvStep = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < kBuffers; ++i) {
            if (FAILED(m_Swap->GetBuffer(i, IID_PPV_ARGS(&m_Back[i])))) return fail("no buffer");
            m_Device->CreateRenderTargetView(m_Back[i].Get(), nullptr, rtv(i));
            if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&m_Alloc[i]))))
                return fail("no allocator");
        }
        if (FAILED(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Alloc[0].Get(),
                                               nullptr, IID_PPV_ARGS(&m_List))))
            return fail("no command list");
        m_List->Close();
        if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence))))
            return fail("no fence");
        m_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    HANDLE waitable() const { return m_Waitable; }

    /// Draw one frame — the flag in @p flagRect when it is up — and present.
    /// Returns the present's number (GetLastPresentCount after it).
    UINT frame(const RECT* flagRect, int64_t& presentCallUs, int64_t& presentUs, int syncInterval)
    {
        const UINT i = m_Swap->GetCurrentBackBufferIndex();
        waitFor(m_FenceValue[i]);
        m_Alloc[i]->Reset();
        m_List->Reset(m_Alloc[i].Get(), nullptr);
        barrier(m_Back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const float background[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        m_List->ClearRenderTargetView(rtv(i), background, 0, nullptr);
        // A bar along the bottom, a step further each frame: every present is
        // a new picture, as a game's is.
        const int barW = std::max(8, m_Width / 64);
        const int x = static_cast<int>((m_Frames * 7) % static_cast<uint64_t>(m_Width - barW + 1));
        const D3D12_RECT bar = {x, m_Height - m_Height / 40, x + barW, m_Height};
        const float grey[4] = {0.35f, 0.35f, 0.35f, 1.0f};
        m_List->ClearRenderTargetView(rtv(i), grey, 1, &bar);
        if (flagRect) {
            // Pure primaries, as LatencyFlag paints: the probe classifies by
            // "clearly blue / white / red" after a chroma-subsampled encode.
            const float colours[3][4] = {{0, 0, 1, 1}, {1, 1, 1, 1}, {1, 0, 0, 1}};
            const LONG w = (flagRect->right - flagRect->left) / 3;
            for (int b = 0; b < 3; ++b) {
                const D3D12_RECT band = {flagRect->left + b * w, flagRect->top,
                                         b == 2 ? flagRect->right : flagRect->left + (b + 1) * w,
                                         flagRect->bottom};
                m_List->ClearRenderTargetView(rtv(i), colours[b], 1, &band);
            }
        }
        barrier(m_Back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_List->Close();
        ID3D12CommandList* lists[] = {m_List.Get()};
        m_Queue->ExecuteCommandLists(1, lists);

        presentCallUs = steadyNowUs();
        const UINT flags = (syncInterval == 0 && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        m_Swap->Present(static_cast<UINT>(syncInterval), flags);
        presentUs = steadyNowUs();
        m_FenceValue[i] = ++m_NextFence;
        m_Queue->Signal(m_Fence.Get(), m_FenceValue[i]);
        ++m_Frames;
        UINT count = 0;
        m_Swap->GetLastPresentCount(&count);
        return count;
    }

    /// The last present the OS says reached the screen, and when; false while
    /// it says nothing yet (or the statistics were disjoint).
    bool shown(UINT& presentCount, int64_t& syncUs)
    {
        DXGI_FRAME_STATISTICS st = {};
        if (FAILED(m_Swap->GetFrameStatistics(&st)) || st.PresentCount == 0) return false;
        presentCount = st.PresentCount;
        syncUs = qpcToSteadyUs(st.SyncQPCTime.QuadPart);
        return true;
    }

    uint64_t frames() const { return m_Frames; }

    void finish()
    {
        if (m_Queue && m_Fence) {
            m_Queue->Signal(m_Fence.Get(), ++m_NextFence);
            waitFor(m_NextFence);
        }
        if (m_FenceEvent) CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

private:
    bool fail(const std::string& why)
    {
        error = why;
        return false;
    }

    ComPtr<IDXGIAdapter1> pickAdapter(IDXGIFactory6* factory, const std::string& want,
                                      const std::wstring& monitorDevice)
    {
        ComPtr<IDXGIAdapter1> a;
        if (want == "warp") {
            factory->EnumWarpAdapter(IID_PPV_ARGS(&a));
            return a;
        }
        ComPtr<IDXGIAdapter1> first;
        for (UINT i = 0;
             factory->EnumAdapters1(i, a.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            const std::string name = narrow(d.Description);
            if (!want.empty()) {
                if (name.find(want) != std::string::npos) return a;
                continue;
            }
            if (!first) first = a;
            // The GPU that drives the screen: no copy across GPUs at present.
            ComPtr<IDXGIOutput> out;
            for (UINT j = 0;
                 a->EnumOutputs(j, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC od = {};
                if (SUCCEEDED(out->GetDesc(&od)) && monitorDevice == od.DeviceName) return a;
            }
        }
        return want.empty() ? first : nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_Rtvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * m_RtvStep;
        return h;
    }

    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        m_List->ResourceBarrier(1, &b);
    }

    void waitFor(uint64_t value)
    {
        if (value == 0 || m_Fence->GetCompletedValue() >= value) return;
        m_Fence->SetEventOnCompletion(value, m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, 1000);
    }

    int m_Width = 0;
    int m_Height = 0;
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_Queue;
    ComPtr<IDXGISwapChain3> m_Swap;
    UINT m_SwapFlags = 0;
    HANDLE m_Waitable = nullptr;
    ComPtr<ID3D12DescriptorHeap> m_Rtvs;
    UINT m_RtvStep = 0;
    ComPtr<ID3D12Resource> m_Back[kBuffers];
    ComPtr<ID3D12CommandAllocator> m_Alloc[kBuffers];
    ComPtr<ID3D12GraphicsCommandList> m_List;
    ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_FenceEvent = nullptr;
    uint64_t m_FenceValue[kBuffers] = {};
    uint64_t m_NextFence = 0;
    uint64_t m_Frames = 0;
};

std::string clickJson(const PendingClick& c, int64_t displayedUs, const char* how)
{
    std::string s =
        "{\"click\":" + std::to_string(c.id) + ",\"downUs\":" + std::to_string(c.downUs) +
        ",\"renderUs\":" + std::to_string(c.renderUs) +
        ",\"presentCallUs\":" + std::to_string(c.presentCallUs) +
        ",\"presentUs\":" + std::to_string(c.presentUs) +
        ",\"presentId\":" + std::to_string(c.presentId) +
        ",\"displayedUs\":" + (displayedUs ? std::to_string(displayedUs) : std::string("null")) +
        ",\"displayed\":\"" + how + "\"}";
    return s;
}

} // namespace

int run(const Options& o, Log& log)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        std::fprintf(stderr, "mw-click-target: no screen\n");
        return 1;
    }
    const Monitor* mon = &monitors.front();
    if (!o.display.empty()) {
        mon = nullptr;
        char* end = nullptr;
        const long idx = std::strtol(o.display.c_str(), &end, 10);
        if (end && !*end && idx >= 0 && idx < static_cast<long>(monitors.size())) {
            mon = &monitors[static_cast<size_t>(idx)];
        } else {
            for (const Monitor& m : monitors)
                if (narrow(m.device.c_str()) == o.display) mon = &m;
        }
        if (!mon) {
            std::fprintf(stderr, "mw-click-target: no screen \"%s\"\n", o.display.c_str());
            return 1;
        }
    }
    const RECT mr = mon->rect;
    const int mw = mr.right - mr.left;
    const int mh = mr.bottom - mr.top;
    // A window over the top 90 %: never the whole screen, so always composed.
    const int ww = mw;
    const int wh = o.fullscreen ? mh : mh * 9 / 10;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    State state;
    state.drawFlag = o.drawFlag;
    g_State = &state;
    // Topmost: a window someone opens on the screen it took would otherwise
    // come over it and take the clicks (09/10: an Explorer window, 60 clicks).
    state.hwnd =
        CreateWindowExW(WS_EX_TOPMOST, kClass, L"MoonlightWeb click target", WS_POPUP | WS_VISIBLE,
                        mr.left, mr.top, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!state.hwnd) {
        std::fprintf(stderr, "mw-click-target: no window (%lu)\n", GetLastError());
        return 1;
    }
    SetForegroundWindow(state.hwnd);

    // The probe clicks wherever the host's cursor is and never moves it: put
    // the cursor on the window, and back whenever something takes it away (a
    // screen that comes or goes moves it). Without this, two passes on 08/10
    // ran with every click landing on another screen: 60 sent, none received.
    RECT windowRect = {};
    GetWindowRect(state.hwnd, &windowRect);
    // And whose window is under it: another one there takes the clicks. Said
    // once each time it changes, and this one raised again.
    HWND coveredBy = nullptr;
    auto keepCursor = [&](const char* why) {
        POINT p = {};
        if (!GetCursorPos(&p) || !PtInRect(&windowRect, p)) {
            log.line("{\"cursor\":\"" + std::string(why) +
                     "\",\"at\":" + std::to_string(steadyNowUs()) + ",\"was\":\"" +
                     std::to_string(p.x) + "," + std::to_string(p.y) + "\"}");
            p = {(windowRect.left + windowRect.right) / 2,
                 (windowRect.top + windowRect.bottom) / 2};
            SetCursorPos(p.x, p.y);
        }
        HWND under = GetAncestor(WindowFromPoint(p), GA_ROOT);
        if (under == state.hwnd) under = nullptr;
        if (under == coveredBy) return;
        coveredBy = under;
        std::string who = "none";
        if (under) {
            DWORD pid = 0;
            GetWindowThreadProcessId(under, &pid);
            wchar_t path[MAX_PATH] = L"";
            DWORD n = MAX_PATH;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (proc) {
                QueryFullProcessImageNameW(proc, 0, path, &n);
                CloseHandle(proc);
            }
            const wchar_t* exe = wcsrchr(path, L'\\');
            wchar_t cls[128] = L"";
            GetClassNameW(under, cls, 128);
            who = narrow(exe ? exe + 1 : path) + " " + narrow(cls);
            SetWindowPos(state.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        log.line("{\"covered\":\"" + jsonText(who) + "\",\"at\":" + std::to_string(steadyNowUs()) +
                 "}");
    };
    keepCursor("placed");

    Renderer r;
    if (!r.init(state.hwnd, o, mon->device, ww, wh)) {
        std::fprintf(stderr, "mw-click-target: %s\n", r.error.c_str());
        DestroyWindow(state.hwnd);
        return 1;
    }
    // The flag at its fraction of the SCREEN, in the window's coordinates.
    RECT flag = {static_cast<LONG>(mw * kFlagLeft), static_cast<LONG>(mh * kFlagTop),
                 static_cast<LONG>(mw * kFlagRight), static_cast<LONG>(mh * kFlagBottom)};

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mon->device.c_str(), ENUM_CURRENT_SETTINGS, &dm);
    log.line("{\"start\":" + std::to_string(steadyNowUs()) + ",\"screen\":\"" +
             jsonText(narrow(mon->device.c_str())) + "\",\"size\":\"" + std::to_string(mw) + "x" +
             std::to_string(mh) + "\",\"hz\":" + std::to_string(dm.dmDisplayFrequency) +
             ",\"adapter\":\"" + jsonText(r.adapterName) + "\",\"mode\":\"" +
             (o.fullscreen ? "fullscreen" : "window") + "\",\"sync\":" +
             std::to_string(o.syncInterval) + ",\"tearing\":" + (r.tearing ? "true" : "false") +
             ",\"continuous\":" + (o.continuous ? "true" : "false") + ",\"fps\":" +
             std::to_string(o.fps) + ",\"react\":\"" + (o.reactAtOnce ? "now" : "frame") +
             "\",\"input\":\"" + (o.inputAfterWait ? "after-wait" : "first") +
             "\",\"flag\":" + (o.drawFlag ? "true" : "false") + "}");

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    const int64_t periodUs = o.continuous && o.fps > 0 ? 1000000 / o.fps : 0;
    const int64_t endUs = steadyNowUs() + int64_t(o.durationS) * 1000000;
    int64_t nextFrameUs = steadyNowUs();
    bool flagShown = false;
    int64_t lastStatsUs = steadyNowUs();
    uint64_t lastStatsFrames = 0;
    int64_t lastCursorUs = steadyNowUs();

    auto pump = [] {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };
    auto drawNow = [&](int64_t now) {
        // The swap chain lets a frame go once the last is queued: a game
        // waits here, never on the GPU. With --sync 1 that is up to a refresh;
        // a click that came meanwhile is read now, before the frame is drawn.
        WaitForSingleObject(r.waitable(), 100);
        if (o.inputAfterWait) {
            pump();
            now = steadyNowUs();
        }
        const bool up = now < state.flagUntilUs;
        const int64_t renderUs = steadyNowUs();
        int64_t callUs = 0, doneUs = 0;
        const UINT id = r.frame(up ? &flag : nullptr, callUs, doneUs, o.syncInterval);
        flagShown = up;
        for (PendingClick& c : state.pending) {
            if (c.presented || !up) continue;
            c.presented = true;
            c.renderUs = renderUs;
            c.presentCallUs = callUs;
            c.presentUs = doneUs;
            c.presentId = id;
        }
        state.redraw = false;
    };

    // A first picture, whatever the mode: the screen is ours from here.
    drawNow(steadyNowUs());

    while (!state.quit && steadyNowUs() < endUs) {
        // Input first, at once, whatever the swap chain is doing.
        pump();
        const int64_t now = steadyNowUs();
        const bool flagChanged = (now < state.flagUntilUs) != flagShown;
        const bool due = o.continuous && now >= nextFrameUs;
        if ((state.redraw && o.reactAtOnce) || due || (flagChanged && !o.continuous)) {
            drawNow(now);
            if (o.continuous)
                nextFrameUs = periodUs > 0 ? std::max(nextFrameUs + periodUs, now) : now;
        }

        // The OS's word on what reached the screen.
        UINT shownCount = 0;
        int64_t syncUs = 0;
        if (!state.pending.empty() && r.shown(shownCount, syncUs)) {
            while (!state.pending.empty() && state.pending.front().presented &&
                   shownCount >= state.pending.front().presentId) {
                const PendingClick& c = state.pending.front();
                if (shownCount == c.presentId)
                    log.line(clickJson(c, syncUs, "exact"));
                else
                    log.line(clickJson(c, 0, "passed"));
                state.pending.pop_front();
            }
        }
        // A click never presented (or never reported) for a second: said, dropped.
        while (!state.pending.empty() && now - state.pending.front().downUs > 1000000) {
            log.line(clickJson(state.pending.front(), 0, "unknown"));
            state.pending.pop_front();
        }

        if (now - lastCursorUs >= 250000) {
            keepCursor("brought back");
            lastCursorUs = now;
        }

        if (now - lastStatsUs >= 5000000) {
            const uint64_t f = r.frames();
            log.line("{\"at\":" + std::to_string(now) + ",\"fps\":" +
                     std::to_string((f - lastStatsFrames) * 1000000 / (now - lastStatsUs)) +
                     ",\"clicks\":" + std::to_string(state.clicks) + "}");
            lastStatsUs = now;
            lastStatsFrames = f;
        }

        // Sleep until the next frame is due, a message comes, or (a click on
        // the screen) the statistics are worth reading again.
        DWORD waitMs = INFINITE;
        HANDLE handles[1] = {timer};
        DWORD count = 0;
        if (!state.pending.empty()) {
            waitMs = 1;
        } else if (o.continuous) {
            const int64_t ahead = nextFrameUs - steadyNowUs();
            if (ahead > 0 && timer) {
                LARGE_INTEGER wake = {};
                wake.QuadPart = -ahead * 10;
                SetWaitableTimerEx(timer, &wake, 0, nullptr, nullptr, nullptr, 0);
                count = 1;
            } else {
                waitMs = 0;
            }
        } else if (flagShown) {
            waitMs = static_cast<DWORD>(std::max<int64_t>(1, (state.flagUntilUs - now) / 1000));
        } else {
            waitMs = static_cast<DWORD>(std::min<int64_t>(1000, (endUs - now) / 1000 + 1));
        }
        MsgWaitForMultipleObjectsEx(count, count ? handles : nullptr, count ? INFINITE : waitMs,
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    r.finish();
    log.line("{\"end\":" + std::to_string(steadyNowUs()) + ",\"clicks\":" +
             std::to_string(state.clicks) + ",\"frames\":" + std::to_string(r.frames()) + "}");
    DestroyWindow(state.hwnd);
    g_State = nullptr;
    if (timer) CloseHandle(timer);
    timeEndPeriod(1);
    return 0;
}

} // namespace clicktarget
