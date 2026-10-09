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

#include "ClickTarget.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* kUsage =
    "mw-click-target — on a click, the latency flag drawn and presented at once\n"
    "\n"
    "  --display <n|name>   the screen: 0 the first, or \\\\.\\DISPLAY5 (default: primary)\n"
    "  --window             a window over the top 90% of the screen (default: covering it)\n"
    "  --space              macOS: full screen in a Space of its own\n"
    "  --sync 0|1           present at once (0, default) or on the refresh (1)\n"
    "  --no-tearing         with --sync 0, wait for the refresh rather than tear\n"
    "  --on-click           draw only when the flag changes (default: continuously)\n"
    "  --no-flag            never draw the flag: a window that only takes the clicks,\n"
    "                       under the host's own flag (with --on-click: one picture)\n"
    "  --fps <n>            continuous frames a second at most, 0 none (default 240)\n"
    "  --react frame        a click waits for the next frame due (default: at once)\n"
    "  --input-first        read the input before waiting for the swap chain, as a\n"
    "                       simple loop does (default: after, as a game tuned for\n"
    "                       latency does)\n"
    "  --adapter <name>     the GPU, by a piece of its name, or warp (default: the\n"
    "                       screen's own)\n"
    "  --duration <s>       quit after that long, 1-7200 (default 600; Escape sooner)\n"
    "  --out <file>         the log, one JSON object a line (default: stdout)\n"
    "\n"
    "Each click: {\"click\", \"downUs\", \"renderUs\", \"presentCallUs\", \"presentUs\",\n"
    "\"presentId\", \"displayedUs\"} (macOS: and \"eventUs\"), on the steady clock\n"
    "MoonlightWeb stamps with.\n";

bool takeInt(const char* s, int& out, int lo, int hi)
{
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (!end || *end || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    clicktarget::Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        bool ok = true;
        if (a == "--display" && next) {
            o.display = argv[++i];
        } else if (a == "--window") {
            o.fullscreen = false;
        } else if (a == "--space") {
            o.fullscreenSpace = true;
        } else if (a == "--sync" && next) {
            ok = takeInt(argv[++i], o.syncInterval, 0, 1);
        } else if (a == "--no-tearing") {
            o.tearing = false;
        } else if (a == "--on-click") {
            o.continuous = false;
        } else if (a == "--no-flag") {
            o.drawFlag = false;
        } else if (a == "--fps" && next) {
            ok = takeInt(argv[++i], o.fps, 0, 1000);
        } else if (a == "--react" && next) {
            const std::string v = argv[++i];
            ok = v == "frame" || v == "now";
            o.reactAtOnce = v == "now";
        } else if (a == "--input-first") {
            o.inputAfterWait = false;
        } else if (a == "--adapter" && next) {
            o.adapter = argv[++i];
        } else if (a == "--duration" && next) {
            ok = takeInt(argv[++i], o.durationS, 1, 7200);
        } else if (a == "--out" && next) {
            o.out = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "mw-click-target: bad argument near \"%s\"\n\n%s", a.c_str(),
                         kUsage);
            return 2;
        }
    }

    clicktarget::Log log;
    if (!log.open(o.out)) {
        std::fprintf(stderr, "mw-click-target: cannot write %s\n", o.out.c_str());
        return 1;
    }
    return clicktarget::run(o, log);
}
