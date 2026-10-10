"""Run decoder-lab.html in a headless Chrome of its own: no window, no
profile shared with any other Chrome, no MoonlightWeb instance.

    python decoder_lab.py [--clips text-1080p,game-1080p] [--mbps 170] [--timing 200]
                          [--chrome-arg=--use-adapter-luid=<luid>] [--show]

Serves the repository and the corpus (/corpus/ -> %USERPROFILE%\\.mw-bench\\
ultra-corpus) on 127.0.0.1, opens the page per clip over the DevTools
protocol, and prints and writes the results (<corpus>/decoder-lab.json).
Needs `pip install websocket-client`.
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request

import websocket

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CORPUS = os.path.join(os.path.expanduser("~"), ".mw-bench", "ultra-corpus")
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?", 1)[0]
        if path.startswith("/corpus/"):
            return os.path.join(CORPUS, os.path.basename(path))
        return super().translate_path(path)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", default="text-1080p,game-1080p,gradient-1080p,checker-1080p,noise-1080p")
    ap.add_argument("--mbps", default="170")
    ap.add_argument("--ref", default="0x10de")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--timing", type=int, default=200)
    ap.add_argument("--warm", type=int, default=0, help="untimed decodes before each timed one")
    ap.add_argument("--stages", default="",
                    help="time some stages alone: dequant, idwt or pack, or joined by + (dequant+idwt: the "
                         "player's decode pass)")
    ap.add_argument("--slices", type=int, default=0,
                    help="feed each frame in this many pieces (WebGPU): checked as decoded, and the timed loop "
                         "times what the last piece leaves and every piece's pass")
    ap.add_argument("--chrome-arg", action="append", default=[])
    ap.add_argument("--show", action="store_true", help="print each frame's line")
    ap.add_argument("--present", action="store_true", help="check the product's presentation path once")
    ap.add_argument("--power", default="high-performance", help="the adapter asked for: high-performance or low-power")
    ap.add_argument("--api", default="webgpu", help="webgpu, or webgl2 for the fallback decoder")
    ap.add_argument("--idwt", type=int, default=3,
                    help="the inverse wavelet's shader (WebGPU): 3, or 2 without the fast load of the tiles inside "
                         "the level, or 1 for the first one (the same values, more slowly)")
    ap.add_argument("--fp16", type=int, default=1,
                    help="the two finest levels in FP16 (WebGPU, shaders 2-3): 1, or 0 for every plane in f32")
    ap.add_argument("--cmp-idwt", type=int, default=1, help="--cmp against this shader (default 1)")
    ap.add_argument("--cmp-fp16", type=int, default=0, help="--cmp against this storage (default f32)")
    ap.add_argument("--split", action="store_true",
                    help="time each stage alone, in a pass of its own: dequant, each level of the inverse wavelet, pack")
    ap.add_argument("--cmp", action="store_true", help="each frame's f32 planes against shader 1's")
    ap.add_argument("--bw", action="store_true", help="the GPU's memory bandwidth, copying and writing 64 MiB")
    # Another machine's Chrome, reached through SSH tunnels: its DevTools port
    # forwarded here (-L), and this server forwarded there (-R) on the same
    # port, so both ends stay on the loopback.
    ap.add_argument("--remote-cdp", type=int, default=0, help="DevTools port of an already running Chrome")
    ap.add_argument("--http-port", type=int, default=0, help="fixed port for the server (remote runs)")
    a = ap.parse_args()

    http_port = a.http_port or free_port()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", http_port), functools.partial(Handler, directory=REPO))
    threading.Thread(target=server.serve_forever, daemon=True).start()

    cdp_port = a.remote_cdp or free_port()
    profile = tempfile.mkdtemp(prefix="mw-pyrowave-lab-")
    chrome = None if a.remote_cdp else subprocess.Popen([
        CHROME, "--headless=new", "--remote-debugging-port=%d" % cdp_port,
        "--user-data-dir=" + profile, "--no-first-run", "--no-default-browser-check",
        "--enable-unsafe-webgpu", "--enable-webgpu-developer-features", "about:blank",
    ] + a.chrome_arg, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    results = []
    try:
        for _ in range(50):
            try:
                targets = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp_port))
                pages = [t for t in targets if t["type"] == "page"]
                # A WebView can list a never attached, empty page first.
                page = next((t for t in pages if '"attached":true' in t.get("description", "")), pages[0])
                break
            except Exception:
                time.sleep(0.2)
        ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=600, suppress_origin=True)
        ids = iter(range(1, 1 << 30))

        def call(method, **params):
            mid = next(ids)
            ws.send(json.dumps({"id": mid, "method": method, "params": params}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get("id") == mid:
                    return msg.get("result", msg)

        for clip in a.clips.split(","):
            for mbps in a.mbps.split(","):
                url = ("http://127.0.0.1:%d/scripts/bench/ultra/decoder-lab.html?clip=%s&mbps=%s&ref=%s"
                       "&frames=%d&timing=%d&stages=%s&warm=%d&power=%s&present=%d&api=%s&slices=%d"
                       "&idwt=%d&fp16=%d&split=%d&cmp=%d&bw=%d&cmpidwt=%d&cmpfp16=%d"
                       % (http_port, clip, mbps, a.ref, a.frames, a.timing, a.stages, a.warm, a.power,
                          1 if a.present else 0, a.api, a.slices, a.idwt, a.fp16, 1 if a.split else 0,
                          1 if a.cmp else 0, 1 if a.bw else 0, a.cmp_idwt, a.cmp_fp16))
                call("Page.navigate", url=url)
                # A slow client (a TV) takes seconds to load the module that sets it.
                for _ in range(120):
                    time.sleep(0.5)
                    ready = call("Runtime.evaluate", returnByValue=True,
                                 expression="location.href === %s && window.__labResult !== undefined"
                                 % json.dumps(url))
                    if ready.get("result", {}).get("value"):
                        break
                r = call("Runtime.evaluate", expression="window.__labResult", awaitPromise=True,
                         returnByValue=True)
                res = r.get("result", {}).get("value") or {"error": json.dumps(r)[:2000]}
                results.append(res)
                if "error" in res:
                    print(clip, mbps, "ERROR", res["error"])
                    continue
                if a.show:
                    for f in res["perFrame"]:
                        print("  ", f)
                g = res.get("gpuMs") or {}
                label = res.get("stages", "") + (" /%d" % res["slices"] if res.get("slices") else "")
                if res.get("idwt") is not None:
                    label += " i%d" % res["idwt"]
                if res.get("fp16") is not None:
                    label += " fp16" if res["fp16"] else " f32"
                print("%-15s %4s Mbit/s %-12s %s  max diff Y %d C %d vs oracle (min PSNR %.1f dB), PSNR-Y vs "
                      "source %.2f dB, ready %s, GPU decode p50 %s ms p99 %s ms%s"
                      % (clip, mbps, label,
                         res["adapter"], res["maxDiff"], res["maxDiffC"], res["minPsnrVsOracle"],
                         res["meanPsnrVsSource"], res["allReady"],
                         "%.3f" % g["p50"] if g else "-", "%.3f" % g["p99"] if g else "-",
                         " ERRORS %s" % res["errors"] if res["errors"] else ""))
                if res.get("piecesSumMs"):
                    print("    by slices, every piece's pass summed: p50 %.3f ms p99 %.3f ms; first start to last end "
                          "p50 %.3f ms" % (res["piecesSumMs"]["p50"], res["piecesSumMs"]["p99"],
                                           res["piecesSpanMs"]["p50"]))
                if res.get("wallMs"):
                    print("    wall clock, decode + present + 1-pixel read-back: p50 %.2f ms p99 %.2f ms"
                          % (res["wallMs"]["p50"], res["wallMs"]["p99"]))
                if res.get("presentMaxDiff") is not None:
                    pg = res.get("presentGpuMs")
                    print("    present path: max RGB diff %d against the oracle's frame%s"
                          % (res["presentMaxDiff"],
                             ", GPU p50 %.3f ms p99 %.3f ms" % (pg["p50"], pg["p99"]) if pg else ""))
                if res.get("planeDiff") is not None:
                    cw = res.get("cmpWith") or {"idwt": 1}
                    print("    idwt %d%s against idwt %d%s: max plane difference %.3g; 8-bit frame: max %d, "
                          "%.3f %% of samples differ"
                          % (res["idwt"], " fp16" if res.get("fp16") else " f32", cw["idwt"],
                             " fp16" if cw.get("fp16") else " f32", res["planeDiff"], res["codeDiff"],
                             res["codesOffPct"]))
                if res.get("split"):
                    sp = res["split"]
                    print("    idwt %d%s, each stage alone, p50 (p99) ms: %s" % (
                        res["idwt"], " fp16" if res.get("fp16") else " f32", ", ".join(
                            "%s %.3f (%.3f)" % (s, v["p50"], v["p99"]) for s, v in sp.items() if v)))
                    levels = [v["p50"] for s, v in sp.items() if s.startswith("idwt") and v]
                    if levels:
                        print("    the levels summed: %.3f ms" % sum(levels))
                if res.get("bandwidth"):
                    print("    bandwidth: copy %(copyGBs).1f GB/s (read + write), write %(writeGBs).1f GB/s"
                          % res["bandwidth"])
    finally:
        if chrome:
            chrome.terminate()
            try:
                chrome.wait(10)
            except subprocess.TimeoutExpired:
                chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    with open(os.path.join(CORPUS, "decoder-lab.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
