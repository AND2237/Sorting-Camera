"""Phase 5 pipeline baseline runner.

Launches the viewer in bench mode, polls the device status endpoint while it
runs, and merges both sides into one JSON record:

    python benchmarks/phase5_pipeline.py --framesize hd --quality 12 --seconds 120

The app records PC-side pipeline metrics (decode, parse, render, frame age,
counters, CPU, working set); this script records the ESP32 side (capture and
delivery fps, capture time, frame bytes, RSSI, recoveries, reset reason) as a
time series, so device and PC numbers can be correlated for the same window.
"""

import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import time
from datetime import datetime

HOST_DEFAULT = "192.168.4.1"
EXE_DEFAULT = r"D:\Desktop\Sorting_Camera\desktop\build\SortingCamera.exe"
QT_BIN_DEFAULT = r"C:\Qt\6.11.2\mingw_64\bin"


def http_get_json(host, path, port=80, timeout=4.0):
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
                   % (path, host)).encode())
        s.settimeout(timeout)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        clen = 0
        for line in head.decode(errors="replace").split("\r\n"):
            if line.lower().startswith("content-length:"):
                clen = int(line.split(":", 1)[1])
        body = rest
        while len(body) < clen:
            chunk = s.recv(4096)
            if not chunk:
                break
            body += chunk
        s.close()
        return json.loads(body[:clen].decode(errors="replace"))
    except Exception:
        return None


def wait_for_device(host, attempts=10, delay=2.0):
    for i in range(attempts):
        st = http_get_json(host, "/api/v1/status")
        if st:
            return st
        time.sleep(delay)
    return None


def median(values):
    vals = [v for v in values if isinstance(v, (int, float))]
    return round(statistics.median(vals), 3) if vals else None


def slice_to_window(series, start_ms, end_ms, launched_elapsed_s):
    """Keep only the device samples that fall inside the app's metrics window."""
    if not isinstance(start_ms, int) or not isinstance(end_ms, int) or start_ms < 0:
        return series
    lo = launched_elapsed_s + start_ms / 1000.0
    hi = launched_elapsed_s + end_ms / 1000.0
    return [s for s in series if isinstance(s.get("elapsed_s"), (int, float))
            and lo <= s["elapsed_s"] <= hi]


def validity(series, app):
    """Detect device reboots and config mismatches; a run that hits either is not clean."""
    ups = [s.get("uptime_s") for s in series if isinstance(s.get("uptime_s"), int)]
    reboots = 0
    for prev, cur in zip(ups, ups[1:]):
        if cur < prev:
            reboots += 1
    problems = []
    if reboots:
        problems.append("device rebooted %d time(s) during the window" % reboots)
    if not app.get("metrics_started"):
        problems.append("app never started measuring (no frames)")
    want = {
        "framesize": (app.get("requested_framesize"), "resolution"),
        "quality": (app.get("requested_quality"), "quality"),
        "xclk_mhz": (app.get("requested_xclk_mhz"), "xclk_mhz"),
        "fb_count": (app.get("requested_fb_count"), "fb_count"),
    }
    status = app.get("device_status") or {}
    applied = {}
    for key, (requested, field) in want.items():
        if requested is None:
            continue
        actual = status.get(field)
        applied[key] = {"requested": requested, "applied": actual}
        if field == "resolution":
            dims = {"qqvga": "160x120", "qvga": "320x240", "vga": "640x480",
                    "svga": "800x600", "xga": "1024x768", "hd": "1280x720",
                    "sxga": "1280x1024", "uxga": "1600x1200"}
            expect = dims.get(str(requested), str(requested))
            if actual != expect:
                problems.append("framesize requested %s but device reports %s"
                                % (expect, actual))
        elif actual != requested:
            problems.append("%s requested %s but device reports %s" % (key, requested, actual))
    if app.get("requested_grab") and status.get("grab_mode") != app.get("requested_grab"):
        problems.append("grab requested %s but device reports %s"
                        % (app.get("requested_grab"), status.get("grab_mode")))
    return {"clean": not problems, "problems": problems, "reboots": reboots,
            "config_applied": applied}


def summarize_device(series):
    def col(key):
        return [s.get(key) for s in series if isinstance(s, dict) and key in s]

    def derived_fps(key):
        vals = col(key)
        times = [s.get("elapsed_s") for s in series if isinstance(s, dict)]
        if len(vals) > 1 and len(times) == len(vals):
            span = times[-1] - times[0]
            if span > 0:
                return round((vals[-1] - vals[0]) / span, 3)
        return None

    recoveries = [s.get("camera_recoveries") for s in series
                  if isinstance(s, dict) and s.get("camera_recoveries") is not None]
    return {
        "samples": len(series),
        "window_s": (series[-1].get("elapsed_s") - series[0].get("elapsed_s"))
        if len(series) > 1 else None,
        "capture_fps": derived_fps("frames_captured"),
        "delivery_fps": derived_fps("frames_delivered"),
        "avg_capture_ms_p50": median(col("avg_capture_ms")),
        "last_frame_bytes_p50": median(col("last_frame_bytes")),
        "rssi_p50": median(col("rssi")),
        "free_heap_min": min([v for v in col("free_heap") if isinstance(v, (int, float))],
                             default=None),
        "capture_failures_delta": (col("capture_failures")[-1] - col("capture_failures")[0])
        if len(col("capture_failures")) > 1 else None,
        "camera_recoveries_delta": (max(recoveries) - min(recoveries)) if recoveries else None,
        "reset_reasons": sorted({s.get("reset_reason") for s in series
                                 if isinstance(s, dict) and s.get("reset_reason") is not None}),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--framesize")
    ap.add_argument("--quality", type=int)
    ap.add_argument("--fb-count", type=int)
    ap.add_argument("--grab")
    ap.add_argument("--xclk", type=int)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--host", default=HOST_DEFAULT)
    ap.add_argument("--exe", default=EXE_DEFAULT)
    ap.add_argument("--qt-bin", default=QT_BIN_DEFAULT)
    ap.add_argument("--out", required=True)
    ap.add_argument("--poll-interval", type=float, default=2.0)
    args = ap.parse_args()

    st = wait_for_device(args.host)
    if not st:
        print("device %s not reachable - is the PC joined to the ESP32-CAM AP?" % args.host)
        return 2
    print("device reachable: %s q=%s fw=%s"
          % (st.get("resolution"), st.get("quality"), st.get("fw_version")), flush=True)

    env = dict(os.environ)
    env["PATH"] = args.qt_bin + os.pathsep + env.get("PATH", "")

    app_json = os.path.join(os.path.dirname(args.out) or ".", "_app_tmp.json")
    cmd = [args.exe, "--bench", str(args.seconds), "--warmup", str(args.warmup),
           "--out", app_json, "--host", args.host]
    if args.framesize:
        cmd += ["--framesize", args.framesize]
    if args.quality is not None:
        cmd += ["--quality", str(args.quality)]
    if args.fb_count is not None:
        cmd += ["--fb-count", str(args.fb_count)]
    if args.grab:
        cmd += ["--grab", args.grab]
    if args.xclk is not None:
        cmd += ["--xclk", str(args.xclk)]
    print("launching: %s" % " ".join(cmd), flush=True)

    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    series = []
    launched_elapsed_s = 0.0
    t0 = time.time()
    while proc.poll() is None:
        if not series:
            launched_elapsed_s = 0.0
        now = datetime.now().isoformat(timespec="seconds")
        sample = http_get_json(args.host, "/api/v1/status")
        if sample:
            sample["t"] = now
            sample["elapsed_s"] = round(time.time() - t0, 1)
            series.append(sample)
        time.sleep(args.poll_interval)
    out = proc.stdout.read() if proc.stdout else ""
    proc.wait()
    print("app exited rc=%s after %.0f s" % (proc.returncode, time.time() - t0), flush=True)
    for line in out.splitlines()[-6:]:
        print("  app| %s" % line)

    if not os.path.exists(app_json):
        print("app did not write %s" % app_json)
        return 3
    with open(app_json, encoding="utf-8") as f:
        app = json.load(f)
    os.remove(app_json)

    window_series = slice_to_window(series, app.get("metrics_start_ms"),
                                    app.get("metrics_end_ms"), launched_elapsed_s)
    final = http_get_json(args.host, "/api/v1/status") or {}
    check = validity(window_series, app)
    record = {
        "meta": {
            "stage": "phase5-baseline",
            "date": datetime.now().isoformat(timespec="seconds"),
            "requested_framesize": args.framesize,
            "requested_quality": args.quality,
            "bench_seconds": args.seconds,
            "warmup_seconds": args.warmup,
            "host": args.host,
            "app_version": app.get("app_version"),
        },
        "validity": check,
        "device_before": st,
        "device_after": final,
        "device_summary": summarize_device(window_series),
        "device_summary_whole_run": summarize_device(series),
        "device_series": series,
        "app": app,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)

    a = app
    d = record["device_summary"]
    st_cfg = a.get("device_status") or {}
    print("\n=== %s q%s fb%s %s xclk%s ==="
          % (args.framesize, args.quality, st_cfg.get("fb_count"), st_cfg.get("grab_mode"),
             st_cfg.get("xclk_mhz")))
    print("  VALIDITY: %s%s" % ("clean" if check["clean"] else "INVALID -> " + "; ".join(check["problems"]),
                                "" if check["clean"] else ""))
    print("  device : capture_fps=%s delivery_fps=%s avg_capture_ms=%s bytes=%s rssi=%s"
          % (d["capture_fps"], d["delivery_fps"], d["avg_capture_ms_p50"],
             d["last_frame_bytes_p50"], d["rssi_p50"]))
    print("  app    : parts_fps=%.2f decoded_fps=%.2f presented_fps=%.2f mbps=%.2f cpu=%.1f%%"
          % (a.get("parts_fps", 0.0), a.get("decoded_fps", 0.0), a.get("presented_fps", 0.0),
             a.get("receive_mbps", 0.0), a.get("cpu_percent", 0.0)))
    print("  timings: decode_us p50=%s p95=%s | parse_us p50=%s p95=%s | render_us p50=%s p95=%s"
          % (a["decode_us"].get("p50"), a["decode_us"].get("p95"),
             a["parse_us"].get("p50"), a["parse_us"].get("p95"),
             a["render_us"].get("p50"), a["render_us"].get("p95")))
    print("  ages_ms: present p50=%s p95=%s | render p50=%s p95=%s"
          % (a["present_age_ms"].get("p50"), a["present_age_ms"].get("p95"),
             a["render_age_ms"].get("p50"), a["render_age_ms"].get("p95")))
    print("  drops  : decode_failures=%s overwritten=%s stale=%s recoveries=%s capture_fail=%s"
          % (a.get("decode_failures"), a.get("frames_overwritten"),
             a.get("stale_dropped"), d.get("camera_recoveries_delta"),
             d.get("capture_failures_delta")))
    print("  saved  : %s" % args.out)
    return 0 if check["clean"] else 1


if __name__ == "__main__":
    sys.exit(main())
