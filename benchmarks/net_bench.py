#!/usr/bin/env python3
"""TCP throughput test client for the ESP32 network-bench firmware.

Pairs with firmware/esp32_cam_stream/main/net_bench.c, which is compiled only
when CONFIG_SORTING_CAM_NET_BENCH is set (see docs/performance-workplan.md,
step 1). Join the camera's Wi-Fi first, then:

    python benchmarks/net_bench.py --mode int   --seconds 20
    python benchmarks/net_bench.py --mode psram --seconds 20
    python benchmarks/net_bench.py --mode cam   --seconds 30      # real JPEG frames

What each mode answers:
    int    how fast can the link carry data that sits in internal RAM
    psram  the same, when every byte is read from external RAM first
    cam    the same, with real camera frames (length-prefixed): the end-to-end
           rate of capture + send with no HTTP server in the path

Only numbers measured on this PC's socket are reported here. The device's own
view of the same run (device_mbps, slow_sends, max_send_ms, free internal RAM,
and the exact sdkconfig the binary was built with) is on the serial console,
line "bench done" / "build cfg". Copy both into the result record: a number
without the build config next to it is not usable (ADR-0015).

Results are written as JSON to benchmarks/results/net-<label>-<timestamp>.json.
"""

import argparse
import json
import platform
import socket
import statistics
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

HOST_DEFAULT = "192.168.4.1"
PORT_DEFAULT = 83
FRAME_MAGIC = b"FRM1"
FRAME_HDR = struct.Struct("<4sI")
RECV_BUF = 256 * 1024


def pct(values, p):
    if not values:
        return None
    xs = sorted(values)
    k = max(0, min(len(xs) - 1, int(round((p / 100.0) * (len(xs) - 1)))))
    return xs[k]


def dist(values):
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "min": min(values),
        "p5": pct(values, 5),
        "p50": pct(values, 50),
        "p95": pct(values, 95),
        "max": max(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.pstdev(values) if len(values) > 1 else 0.0,
    }


class FrameParser:
    """Counts length-prefixed frames out of a TCP byte stream without copying
    the payload: it only needs the 8-byte headers, so payload bytes are skipped
    by count. Anything that is not a valid header is counted as a protocol error
    and stops parsing - a desynchronised stream cannot be trusted afterwards."""

    def __init__(self):
        self.hdr = bytearray()
        self.skip = 0
        self.cur_len = 0
        self.frames = []  # (arrival_time, payload_len)
        self.errors = 0
        self.desync = False

    def feed(self, data, now):
        i = 0
        n = len(data)
        while i < n and not self.desync:
            if self.skip:
                take = min(self.skip, n - i)
                self.skip -= take
                i += take
                if self.skip == 0:
                    self.frames.append((now, self.cur_len))
                continue
            need = FRAME_HDR.size - len(self.hdr)
            take = min(need, n - i)
            self.hdr += data[i:i + take]
            i += take
            if len(self.hdr) == FRAME_HDR.size:
                magic, length = FRAME_HDR.unpack(bytes(self.hdr))
                self.hdr.clear()
                if magic != FRAME_MAGIC or length == 0 or length > 4 * 1024 * 1024:
                    self.errors += 1
                    self.desync = True
                    return
                self.cur_len = length
                self.skip = length


def run(args):
    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.connect_timeout)
    except OSError as exc:
        raise SystemExit(f"cannot connect to {args.host}:{args.port} ({exc}). Is the PC joined to the "
                         "camera's Wi-Fi, and is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y?")
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.settimeout(args.recv_timeout)
    req = f"BENCH {args.mode} {args.seconds} {args.chunk}\n".encode()
    sock.sendall(req)

    buf = bytearray(RECV_BUF)
    view = memoryview(buf)
    parser = FrameParser() if args.mode == "cam" else None

    total = 0
    t_first = None
    t_last = None
    per_sec = {}
    error = None
    try:
        while True:
            n = sock.recv_into(buf)
            now = time.perf_counter()
            if n == 0:
                break
            if t_first is None:
                t_first = now
            t_last = now
            total += n
            sec = int(now - t_first)
            per_sec[sec] = per_sec.get(sec, 0) + n
            if parser is not None and not parser.desync:
                parser.feed(view[:n], now)
    except socket.timeout:
        error = "recv timeout (device stopped sending)"
    except OSError as exc:
        error = f"socket error: {exc}"
    finally:
        sock.close()

    if t_first is None or t_last is None:
        raise SystemExit(f"no data received ({error or 'connection closed immediately'}); "
                         "is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y, and is the "
                         "mode valid for this build (mode cam needs the camera enabled)?")

    duration = max(t_last - t_first, 1e-6)
    # Whole seconds only, and drop the warm-up seconds and the final partial one.
    last_full = int(duration)
    secs = [s for s in range(args.warmup, last_full) if s in per_sec]
    mbps_series = [per_sec[s] * 8 / 1e6 for s in secs]
    median = pct(mbps_series, 50) if mbps_series else None
    stalled = sum(1 for v in mbps_series if median and v < 0.5 * median)

    result = {
        "meta": {
            "date": datetime.now().isoformat(timespec="seconds"),
            "label": args.label,
            "host": args.host,
            "mode": args.mode,
            "seconds_requested": args.seconds,
            "chunk": args.chunk,
            "warmup_s": args.warmup,
            "pc": {"os": platform.platform(), "cpu": platform.processor(),
                   "python": platform.python_version()},
            "note": "device-side numbers and the firmware build config are on the serial console",
        },
        "received": {
            "bytes": total,
            "duration_s": duration,
            "mean_mbps_whole_run": total * 8 / duration / 1e6,
            "per_second_mbps": dist(mbps_series),
            "stalled_seconds_below_half_median": stalled,
            "error": error,
        },
    }

    if parser is not None:
        frames = parser.frames
        sizes = [length for _, length in frames]
        intervals = [(frames[i][0] - frames[i - 1][0]) * 1000.0 for i in range(1, len(frames))]
        in_window = [t for t, _ in frames if t_first + args.warmup <= t <= t_first + last_full]
        window_s = max(0.0, last_full - args.warmup)
        result["frames"] = {
            "count": len(frames),
            "fps_after_warmup": (len(in_window) / window_s) if window_s > 0 else None,
            "frame_bytes": dist(sizes),
            "interval_ms": dist(intervals),
            "protocol_errors": parser.errors,
            "desynchronised": parser.desync,
        }

    return result


def print_report(r):
    rec = r["received"]
    ps = rec["per_second_mbps"]
    print(f"\n=== net_bench  mode={r['meta']['mode']}  chunk={r['meta']['chunk']}  "
          f"label={r['meta']['label']} ===")
    if rec["duration_s"] < 0.5:
        print(f"received      {rec['bytes'] / 1e6:.2f} MB in under 0.5 s -> rate not meaningful")
    else:
        print(f"received      {rec['bytes'] / 1e6:.2f} MB in {rec['duration_s']:.2f} s "
              f"-> {rec['mean_mbps_whole_run']:.2f} Mbps whole run")
    if ps.get("n"):
        print(f"per second    n={ps['n']}  min {ps['min']:.2f} | p5 {ps['p5']:.2f} | "
              f"p50 {ps['p50']:.2f} | p95 {ps['p95']:.2f} | max {ps['max']:.2f} | "
              f"mean {ps['mean']:.2f} (sd {ps['stdev']:.2f}) Mbps")
        print(f"stalls        {rec['stalled_seconds_below_half_median']} second(s) below "
              "half the median rate")
    else:
        print("per second    not enough full seconds after warm-up; increase --seconds")
    if rec["error"]:
        print(f"ERROR         {rec['error']}")
    if rec["duration_s"] < r["meta"]["seconds_requested"] * 0.8:
        print(f"WARNING       the stream lasted {rec['duration_s']:.2f} s of the {r['meta']['seconds_requested']} s "
              "requested; the device stopped early, so these rates are not a valid measurement")
    fr = r.get("frames")
    if fr:
        fb = fr["frame_bytes"]
        iv = fr["interval_ms"]
        print(f"frames        {fr['count']}  fps(after warm-up) "
              f"{fr['fps_after_warmup'] if fr['fps_after_warmup'] is None else round(fr['fps_after_warmup'], 2)}")
        if fb.get("n"):
            print(f"frame bytes   p50 {fb['p50']:.0f} | p95 {fb['p95']:.0f} | max {fb['max']:.0f}")
        if iv.get("n"):
            print(f"interval ms   p50 {iv['p50']:.1f} | p95 {iv['p95']:.1f} | max {iv['max']:.1f}")
        if fr["protocol_errors"]:
            print(f"PROTOCOL ERRORS {fr['protocol_errors']} (stream desynchronised)")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=HOST_DEFAULT)
    ap.add_argument("--port", type=int, default=PORT_DEFAULT)
    ap.add_argument("--mode", choices=["int", "psram", "cam"], default="int")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--chunk", type=int, default=8192,
                    help="bytes per send() call on the device (512..32768)")
    ap.add_argument("--warmup", type=int, default=2, help="seconds excluded from per-second stats")
    ap.add_argument("--label", default="run",
                    help="free text naming the firmware config under test, e.g. baseline or block")
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    ap.add_argument("--recv-timeout", type=float, default=8.0)
    ap.add_argument("--out-dir", default=str(Path(__file__).resolve().parent / "results"))
    ap.add_argument("--no-save", action="store_true")
    args = ap.parse_args()

    if not 1 <= args.seconds <= 300:
        ap.error("--seconds must be 1..300")
    if not 512 <= args.chunk <= 32768:
        ap.error("--chunk must be 512..32768")
    if args.warmup >= args.seconds - 1:
        ap.error("--warmup must leave at least two measured seconds")

    result = run(args)
    print_report(result)
    if not args.no_save:
        out = Path(args.out_dir)
        out.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        path = out / f"net-{args.label}-{args.mode}-{stamp}.json"
        path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"saved {path}")


if __name__ == "__main__":
    sys.exit(main())
