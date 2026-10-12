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

No serial console is needed. Before and after every run this client sends the
device an "INFO" request and stores the reply in the result JSON under "device":
the exact sdkconfig the binary was built with, the radio and flash mode read
back at run time (flash_io shows "qio" only if QIO is really active), the reset
reason and uptime (a reboot or brownout during a run is detected and the run is
marked INVALID), and the device's own statistics of the run (device Mbps, slow
sends, longest send, free internal RAM). The PC's rate and the device's rate must
agree within 5%, or the run is marked INVALID. A number without its build config
next to it is not usable (ADR-0015); here the two travel in the same file.

    python benchmarks/net_bench.py --info-only      # just read the device state

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


HEAD_BYTES = 1024


def jpeg_dims(head):
    """(width, height) from the first SOF marker of a JPEG header, or None."""
    n = len(head)
    if n < 4 or head[0] != 0xFF or head[1] != 0xD8:
        return None
    i = 2
    while i + 9 < n:
        if head[i] != 0xFF:
            return None
        m = head[i + 1]
        if m in (0xC0, 0xC1, 0xC2):
            return ((head[i + 7] << 8) | head[i + 8], (head[i + 5] << 8) | head[i + 6])
        if m == 0xFF:
            i += 1
        elif m == 0xD8 or m == 0x01 or 0xD0 <= m <= 0xD7:
            i += 2
        else:
            i += 2 + ((head[i + 2] << 8) | head[i + 3])
    return None


class FrameParser:
    """Counts length-prefixed frames out of a TCP byte stream without keeping the
    payload: payload bytes are skipped by count. For every frame it keeps only the
    first HEAD_BYTES and the last two bytes, to check that the frame is a complete
    JPEG (starts FFD8, ends FFD9) and to read its real pixel size from the SOF
    marker. Optionally keeps ONE whole frame (a sample) for inspection. Anything
    that is not a valid header is a protocol error and stops parsing - a
    desynchronised stream cannot be trusted afterwards."""

    def __init__(self, want_sample=False, sample_after=5):
        self.hdr = bytearray()
        self.skip = 0
        self.cur_len = 0
        self.frames = []  # (arrival_time, payload_len)
        self.integrity = []  # (soi_ok, eoi_ok, (w, h) or None), parallel to frames
        self.errors = 0
        self.desync = False
        self.head = bytearray()
        self.tail = b""
        self.want_sample = want_sample
        self.sample_after = sample_after
        self.sample = None
        self.keep = None

    def feed(self, data, now):
        i = 0
        n = len(data)
        while i < n and not self.desync:
            if self.skip:
                take = min(self.skip, n - i)
                chunk = data[i:i + take]
                if len(self.head) < HEAD_BYTES:
                    self.head += chunk[:HEAD_BYTES - len(self.head)]
                self.tail = (self.tail + bytes(chunk[-2:]))[-2:]
                if self.keep is not None:
                    self.keep += chunk
                self.skip -= take
                i += take
                if self.skip == 0:
                    self.frames.append((now, self.cur_len))
                    head = bytes(self.head)
                    soi = len(head) >= 2 and head[0] == 0xFF and head[1] == 0xD8
                    self.integrity.append((soi, self.tail == b"\xff\xd9", jpeg_dims(head)))
                    if self.keep is not None:
                        self.sample = bytes(self.keep)
                        self.keep = None
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
                self.head = bytearray()
                self.tail = b""
                if self.want_sample and self.sample is None and len(self.frames) >= self.sample_after:
                    self.keep = bytearray()


RESET_REASON_NAMES = {0: "unknown", 1: "power-on", 2: "external pin", 3: "software", 4: "panic",
                      5: "interrupt watchdog", 6: "task watchdog", 7: "other watchdog",
                      8: "deep sleep", 9: "BROWNOUT", 10: "sdio", 11: "usb", 12: "jtag",
                      13: "efuse", 14: "power glitch", 15: "cpu lockup"}


def query_info(host, port, timeout=6.0):
    """Asks the bench firmware for its INFO line. Returns (dict, None) or (None, error)."""
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
    except OSError as exc:
        return None, f"cannot connect ({exc})"
    try:
        sock.settimeout(timeout)
        sock.sendall(b"INFO\n")
        data = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 16384:
                return None, "reply too long"
    except OSError as exc:
        return None, f"socket error ({exc})"
    finally:
        sock.close()
    try:
        return json.loads(data.decode("utf-8", "replace").strip()), None
    except ValueError as exc:
        return None, f"reply is not JSON ({exc}): {bytes(data[:80])!r}"


def check_device(pre, post, result):
    """Cross-checks the device's own account of the run against the PC's.
    Returns a list of problems; any problem makes the run INVALID."""
    problems = []
    if pre is None or post is None:
        return ["device INFO unavailable before or after the run"]
    if post.get("uptime_s", 0) < pre.get("uptime_s", 0):
        problems.append(
            f"DEVICE REBOOTED during the run (uptime {pre.get('uptime_s')} s -> "
            f"{post.get('uptime_s')} s, reset reason now "
            f"{RESET_REASON_NAMES.get(post.get('reset_reason'), post.get('reset_reason'))})")
        return problems
    last = post.get("last", {})
    if last.get("runs") != pre.get("last", {}).get("runs", 0) + 1:
        problems.append("device run counter did not advance by exactly one; the INFO reply does "
                        "not describe this run")
        return problems
    if not last.get("finished"):
        problems.append("device reports the run as unfinished")
    if not last.get("ok"):
        problems.append("device reports a send failure during the run")
    pc = result["received"]["mean_mbps_whole_run"]
    dev = last.get("mbps") or 0.0
    if dev > 0 and pc > 0:
        diff = abs(pc - dev) / dev * 100.0
        result["device_vs_pc_mbps_diff_pct"] = diff
        if diff > 5.0:
            problems.append(f"PC rate {pc:.2f} Mbps and device rate {dev:.2f} Mbps differ by "
                            f"{diff:.1f}% (limit 5%)")
    return problems


def print_info(info, title):
    b = info.get("build", {})
    r = info.get("runtime", {})
    print(f"--- {title} ---")
    print(f"firmware      cpu {b.get('cpu_mhz')} MHz | flash {b.get('flash_freq')} cfg={b.get('flash_mode_cfg')} "
          f"RUNTIME io={r.get('flash_io')} id={r.get('flash_id')} size={r.get('flash_size')} | "
          f"psram {b.get('psram_mhz')} MHz | camera {'on' if info.get('camera_enabled') else 'off'}")
    print(f"lwip/wifi     tcp snd/wnd {b.get('tcp_snd_buf')}/{b.get('tcp_wnd')} | wifi rx/tx buf "
          f"{b.get('wifi_rx_buf')}/{b.get('wifi_tx_buf')} | iram wifi={b.get('wifi_iram')} "
          f"rx={b.get('wifi_rx_iram')} lwip={b.get('lwip_iram')} | freertos in flash={b.get('freertos_in_flash')}")
    print(f"radio         protocol bitmap {r.get('protocol_bitmap')} (b=1 g=2 n=4) | {r.get('bandwidth')} | "
          f"channel {r.get('channel')} | max tx power {r.get('max_tx_power_q')} (x0.25 dBm)")
    print(f"health        uptime {info.get('uptime_s')} s | reset reason "
          f"{RESET_REASON_NAMES.get(info.get('reset_reason'), info.get('reset_reason'))} | free internal "
          f"{r.get('free_internal')} (min ever {r.get('min_free_internal')}) | free psram {r.get('free_spiram')}")


def run(args):
    pre_info, pre_err = query_info(args.host, args.port)
    if pre_info is None:
        print(f"WARNING       no INFO before the run: {pre_err}")
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
    sample_path = getattr(args, "sample_path", None)
    parser = FrameParser(want_sample=bool(sample_path)) if args.mode == "cam" else None

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
        integ = parser.integrity
        dims_count = {}
        for _, _, d in integ:
            key = f"{d[0]}x{d[1]}" if d else "unparsed"
            dims_count[key] = dims_count.get(key, 0) + 1
        nint = len(integ)
        result["frames"]["jpeg"] = {
            "frames_checked": nint,
            "soi_ok_pct": (100.0 * sum(1 for a, _, _ in integ if a) / nint) if nint else None,
            "eoi_ok_pct": (100.0 * sum(1 for _, b, _ in integ if b) / nint) if nint else None,
            "dims": dims_count,
            "dominant_dims": max(dims_count, key=dims_count.get) if dims_count else None,
            "sample": None,
        }
        if sample_path and parser.sample:
            Path(sample_path).parent.mkdir(parents=True, exist_ok=True)
            Path(sample_path).write_bytes(parser.sample)
            result["frames"]["jpeg"]["sample"] = str(sample_path)

    # Settle, then ask the device for its own account of the run.
    time.sleep(0.5)
    post_info, post_err = query_info(args.host, args.port)
    result["device"] = {"before": pre_info, "after": post_info,
                        "before_error": pre_err, "after_error": post_err}
    problems = check_device(pre_info, post_info, result)
    result["valid"] = not problems
    result["invalid_reasons"] = problems
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
        jp = fr.get("jpeg")
        if jp and jp.get("frames_checked"):
            print(f"jpeg          actual size {jp['dims']} | start ok {jp['soi_ok_pct']:.1f}% | "
                  f"end marker ok {jp['eoi_ok_pct']:.1f}%" + (f" | sample {jp['sample']}" if jp.get("sample") else ""))
        if fr["protocol_errors"]:
            print(f"PROTOCOL ERRORS {fr['protocol_errors']} (stream desynchronised)")
    dev = r.get("device", {})
    after = dev.get("after")
    if after:
        last = after.get("last", {})
        print(f"device says   {last.get('mbps')} Mbps | {last.get('frames')} frames "
              f"({last.get('fps')} fps) | slow_sends {last.get('slow_sends')} | max_send "
              f"{last.get('max_send_ms')} ms | free internal now {last.get('free_internal')} "
              f"(min ever {last.get('min_free_internal')})")
        if "device_vs_pc_mbps_diff_pct" in r:
            print(f"cross-check   PC vs device rate differ by {r['device_vs_pc_mbps_diff_pct']:.1f}%")
        print_info(after, "device state after the run")
    elif dev.get("after_error"):
        print(f"device INFO   unavailable: {dev['after_error']}")
    if r.get("valid"):
        print("RESULT        VALID")
    else:
        print("RESULT        INVALID")
        for why in r.get("invalid_reasons", []):
            print(f"              - {why}")
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
    ap.add_argument("--info-only", action="store_true",
                    help="only read the device's INFO (build config, radio, flash mode, reset reason)")
    args = ap.parse_args()

    if args.info_only:
        info, err = query_info(args.host, args.port)
        if info is None:
            raise SystemExit(f"INFO failed: {err}. Is the PC joined to the camera's Wi-Fi and the "
                             "board flashed with CONFIG_SORTING_CAM_NET_BENCH=y?")
        print_info(info, f"device state ({args.host}:{args.port})")
        print(json.dumps(info))
        return 0

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
