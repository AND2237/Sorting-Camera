#!/usr/bin/env python3
"""Phase 3 transport benchmark: single run, one transport.

Measures receive FPS, interval/byte distributions, drops/corruption,
RTT-calibrated device->PC latency estimate, and PC CPU usage.
Writes one JSON result file. Never fabricates: every number here is
sampled from live sockets/clocks/status API on this machine.
"""

import argparse
import ctypes
import json
import platform
import socket
import statistics
import struct
import subprocess
import sys
import time
import zlib
from ctypes import wintypes
from pathlib import Path

FRAME_HEADER = struct.Struct("<4sBBHIQHHBBIHI")
assert FRAME_HEADER.size == 36
UDP_HEADER = struct.Struct("<4sIHHI")
assert UDP_HEADER.size == 16

TCP_PORT = 82
UDP_PORT = 8500
HTTP_PORT = 81
STATUS_URL = "http://{host}:80/api/v1/status"


def pct(values, p):
    if not values:
        return None
    xs = sorted(values)
    k = max(0, min(len(xs) - 1, int(round((p / 100.0) * (len(xs) - 1)))))
    return xs[k]


def summarise(values):
    if not values:
        return {"min": None, "p50": None, "p95": None, "max": None, "mean": None}
    return {
        "min": min(values),
        "p50": pct(values, 50),
        "p95": pct(values, 95),
        "max": max(values),
        "mean": statistics.fmean(values),
    }


def round_stats(stats, digits=2):
    return {k: (round(v, digits) if isinstance(v, (int, float)) else None)
            for k, v in stats.items()}


class CpuMeter:
    def __init__(self):
        self.k32 = ctypes.windll.kernel32

        class FileTime(ctypes.Structure):
            _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]

        self._ft = FileTime
        self.k32.GetSystemTimes.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.c_void_p]
        self.k32.GetSystemTimes.restype = wintypes.BOOL
        self.k32.GetCurrentProcess.restype = ctypes.c_void_p
        self.k32.GetProcessTimes.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.c_void_p]
        self.k32.GetProcessTimes.restype = wintypes.BOOL

    @staticmethod
    def _i(ft):
        return (ft.high << 32) | ft.low

    def _system(self):
        idle, user, kern = self._ft(), self._ft(), self._ft()
        ok = self.k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(user),
                                     ctypes.byref(kern))
        if not ok:
            raise OSError("GetSystemTimes failed")
        return self._i(idle), self._i(user) + self._i(kern)

    def _proc(self):
        h = self.k32.GetCurrentProcess()
        created, exit_, kernel, user = self._ft(), self._ft(), self._ft(), self._ft()
        ok = self.k32.GetProcessTimes(h, ctypes.byref(created), ctypes.byref(exit_),
                                      ctypes.byref(kernel), ctypes.byref(user))
        if not ok:
            raise OSError("GetProcessTimes failed")
        return self._i(kernel) + self._i(user)

    def snapshot(self):
        return {"system": self._system(), "proc": self._proc(),
                "wall": time.perf_counter()}

    @staticmethod
    def deltas(a, b):
        s_idle_a, s_tot_a = a["system"]
        s_idle_b, s_tot_b = b["system"]
        s_wall = (b["wall"] - a["wall"]) * 1e7
        sys_pct = None
        if s_tot_b > s_tot_a and s_wall > 0:
            sys_pct = 100.0 * (1.0 - ((s_idle_b - s_idle_a) / (s_tot_b - s_tot_a)))
        proc_pct = None
        if s_wall > 0:
            proc_pct = 100.0 * ((b["proc"] - a["proc"]) / s_wall)
            proc_pct = max(0.0, proc_pct)
        return {"system_cpu_pct": round(sys_pct, 2) if sys_pct is not None else None,
                "process_cpu_pct_onecore": round(proc_pct, 2) if proc_pct is not None else None}


def http_get_json(host, url_path, timeout=4):
    import urllib.request
    with urllib.request.urlopen(f"http://{host}:80{url_path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


def device_sync(host, samples=5):
    """RTT-calibrated clock offset: offset = device_us - pc_us (midpoint)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    offsets, rtts = [], []
    try:
        for _ in range(samples):
            t1 = int(time.time() * 1e6)
            s.sendto(b"SCSYNC" + struct.pack("<Q", t1), (host, UDP_PORT))
            try:
                data, _ = s.recvfrom(64)
            except socket.timeout:
                continue
            t3 = int(time.time() * 1e6)
            if len(data) == 21 and data[:5] == b"SCSYR":
                r_t1, t2 = struct.unpack("<QQ", data[5:])
                if r_t1 == t1:
                    offsets.append((t1 + (t3 - t1) // 2) - t2)
                    rtts.append((t3 - t1) / 1000.0)
    finally:
        s.close()
    if not offsets:
        return None, []
    return statistics.median(offsets), rtts


class StatsCollector:
    def __init__(self, warmup_end_pc):
        self.warmup_end = warmup_end_pc
        self.frame_recv_pc = []
        self.frame_dev_us = []
        self.frame_bytes = []
        self.drops = {"seq_gap": 0, "incomplete": 0, "crc_fail": 0, "bad": 0}
        self.bytes_total = 0

    def add_frame(self, pc_us, dev_us, size):
        if pc_us >= self.warmup_end:
            self.frame_recv_pc.append(pc_us)
            if dev_us is not None:
                self.frame_dev_us.append(dev_us)
            self.frame_bytes.append(size)
        self.bytes_total += size

    def report(self, duration_s, warmup_s, offset_us, process_note=""):
        n = len(self.frame_recv_pc)
        window = duration_s - warmup_s
        fps_series = []
        if n >= 2:
            t0 = self.frame_recv_pc[0]
            secs = max(1, int((self.frame_recv_pc[-1] - t0) / 1e6))
            buckets = [0] * secs
            for t in self.frame_recv_pc:
                buckets[min(secs - 1, int((t - t0) / 1e6))] += 1
            fps_series = buckets
        intervals = [(b - a) / 1000.0
                     for a, b in zip(self.frame_recv_pc, self.frame_recv_pc[1:])]
        latency = []
        if offset_us is not None and self.frame_dev_us:
            pairs = list(zip(self.frame_recv_pc, self.frame_dev_us))[-n:]
            latency = [(recv - (dev + offset_us)) / 1000.0
                       for recv, dev in pairs]
        fps_mean = None
        if n >= 2:
            span = (self.frame_recv_pc[-1] - self.frame_recv_pc[0]) / 1e6
            if span > 0:
                fps_mean = round((n - 1) / span, 2)
        return {
            "frames_ok": n,
            "fps": {
                "mean": fps_mean,
                "per_second_median": pct(fps_series, 50),
                "per_second_p95": pct(fps_series, 95),
            },
            "interval_ms": round_stats(summarise(intervals)),
            "frame_bytes": round_stats(summarise(self.frame_bytes), 1),
            "throughput_mbps": round(self.bytes_total * 8 / window / 1e6, 2)
            if window > 0 else None,
            "drops": self.drops,
            "latency_ms": round_stats(summarise(latency)) if latency else None,
            "process_note": process_note,
        }


def run_http(host, dur_s, stats, stop_flag):
    s = socket.create_connection((host, HTTP_PORT), timeout=10)
    s.settimeout(1.0)
    s.sendall(f"GET /stream HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
    buf = b""
    state = "headers"
    chunk_left = 0
    msg = b""
    deadline = time.time() + dur_s
    cur_len = 0
    cur_cap = None
    body = b""
    try:
        while time.time() < deadline and not stop_flag["stop"]:
            try:
                data = s.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
            progress = True
            while progress:
                progress = False
                if state == "headers":
                    i = buf.find(b"\r\n\r\n")
                    if i >= 0:
                        buf = buf[i + 4:]
                        state = "chunk_size"
                        progress = True
                elif state == "chunk_size":
                    i = buf.find(b"\r\n")
                    if i >= 0:
                        line = buf[:i].split(b";")[0].strip()
                        chunk_left = int(line, 16)
                        buf = buf[i + 2:]
                        if chunk_left == 0:
                            return
                        state = "chunk_data"
                        progress = True
                elif state == "chunk_data":
                    if len(buf) >= chunk_left:
                        msg += buf[:chunk_left]
                        buf = buf[chunk_left:]
                        state = "chunk_end"
                        progress = True
                elif state == "chunk_end":
                    if len(buf) >= 2:
                        buf = buf[2:]
                        state = "chunk_size"
                        progress = True
                # multipart parse of msg
                while True:
                    j = msg.find(b"--FRAME")
                    if j < 0:
                        if len(msg) > 64:
                            msg = msg[-6:]
                        break
                    h = msg.find(b"\r\n\r\n", j)
                    if h < 0:
                        break
                    hdr = msg[j:h]
                    cl = None
                    cap = None
                    for line in hdr.split(b"\r\n"):
                        if line.lower().startswith(b"content-length:"):
                            cl = int(line.split(b":")[1].strip())
                        elif line.lower().startswith(b"x-capture-us:"):
                            cap = int(line.split(b":")[1].strip())
                    if cl is None:
                        msg = msg[h + 4:]
                        continue
                    if len(msg) < h + 4 + cl:
                        break
                    payload = msg[h + 4:h + 4 + cl]
                    msg = msg[h + 4 + cl:]
                    pc = time.time() * 1e6
                    if payload[:2] == b"\xff\xd8":
                        stats.add_frame(pc, cap, len(payload))
                    else:
                        stats.drops["bad"] += 1
                    progress = True
    finally:
        s.close()


def run_tcp(host, dur_s, stats, stop_flag):
    s = socket.create_connection((host, TCP_PORT), timeout=10)
    s.settimeout(1.0)
    buf = b""
    expected_seq = None
    deadline = time.time() + dur_s
    try:
        while time.time() < deadline and not stop_flag["stop"]:
            try:
                data = s.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
            while True:
                if len(buf) < 4:
                    break
                if buf[:4] != b"SCAM":
                    k = buf.find(b"SCAM")
                    if k < 0:
                        stats.drops["bad"] += len(buf)
                        buf = b""
                        break
                    stats.drops["bad"] += k
                    buf = buf[k:]
                if len(buf) < FRAME_HEADER.size:
                    break
                (magic, ver, hdr_len, dev_id, seq, ts, w, hgt,
                 pixfmt, comp, plen, flags, crc) = FRAME_HEADER.unpack_from(buf)
                if ver != 1 or hdr_len != 36 or plen > 4_000_000:
                    stats.drops["bad"] += 1
                    buf = buf[4:]
                    continue
                if len(buf) < hdr_len + plen:
                    break
                payload = buf[hdr_len:hdr_len + plen]
                buf = buf[hdr_len + plen:]
                pc = time.time() * 1e6
                if expected_seq is not None and seq > expected_seq:
                    stats.drops["seq_gap"] += seq - expected_seq
                expected_seq = seq + 1
                if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
                    stats.drops["crc_fail"] += 1
                    continue
                stats.add_frame(pc, ts, len(payload))
    finally:
        s.close()


def run_udp(host, dur_s, stats, stop_flag):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.settimeout(0.5)
    deadline = time.time() + dur_s
    next_start = 0.0
    partial = {}
    known = set()
    max_seq = -1
    min_seq = -1
    lost_before_reset = 0

    def finalise():
        if max_seq < 0 or min_seq < 0:
            return 0
        return max(0, max_seq - min_seq + 1 - len(known))

    try:
        while time.time() < deadline and not stop_flag["stop"]:
            now = time.time()
            if now >= next_start:
                s.sendto(b"SCSTART1", (host, UDP_PORT))
                next_start = now + 1.0
            try:
                data, _ = s.recvfrom(2048)
            except socket.timeout:
                data = None
            if data and len(data) >= UDP_HEADER.size:
                magic, seq, idx, count, crc = UDP_HEADER.unpack_from(data)
                if magic == b"SCU1" and 0 < count <= 4096 and idx < count:
                    if max_seq >= 0 and seq < max_seq - 64:
                        lost_before_reset += finalise()
                        known.clear()
                        partial.clear()
                        max_seq = -1
                        min_seq = -1
                    max_seq = max(max_seq, seq)
                    min_seq = seq if min_seq < 0 else min(min_seq, seq)
                    entry = partial.setdefault(seq, {"frags": [None] * count,
                                                     "crc": crc, "t0": time.time()})
                    entry["frags"][idx] = data[UDP_HEADER.size:]
                    if all(f is not None for f in entry["frags"]):
                        payload = b"".join(entry["frags"])
                        del partial[seq]
                        known.add(seq)
                        pc = time.time() * 1e6
                        if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
                            stats.drops["crc_fail"] += 1
                        else:
                            stats.add_frame(pc, None, len(payload))
                else:
                    stats.drops["bad"] += 1
            now = time.time()
            for q in [k for k, e in partial.items() if now - e["t0"] > 2.0]:
                del partial[q]
                stats.drops["incomplete"] += 1
                known.add(q)
        stats.drops["incomplete"] += len(partial)
        known.update(partial)
        stats.drops["seq_gap"] = lost_before_reset + finalise()
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--transport", required=True, choices=["http", "tcp", "udp"])
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--duration", type=int, default=120)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    host = args.host
    stop_flag = {"stop": False}

    status_pre = http_get_json(host, "/api/v1/status")
    if status_pre.get("stream_clients") or status_pre.get("tcp_clients"):
        print("ERROR: a stream client is already connected", file=sys.stderr)
        sys.exit(2)

    offset_us, rtts = device_sync(host)
    print(f"sync offset={offset_us} us rtt_ms={[round(r,2) for r in rtts]}")

    cpu = CpuMeter()
    cpu_a = cpu.snapshot()
    stats = StatsCollector(time.time() * 1e6 + args.warmup * 1e6)
    run_at = time.strftime("%Y-%m-%dT%H:%M:%S")

    runner = {"http": run_http, "tcp": run_tcp, "udp": run_udp}[args.transport]
    print(f"run {args.transport} for {args.duration}s (warmup {args.warmup}s)")
    runner(host, args.duration, stats, stop_flag)
    cpu_b = cpu.snapshot()

    time.sleep(1.0)
    status_post = http_get_json(host, "/api/v1/status")

    report = stats.report(args.duration, args.warmup, offset_us)
    result = {
        "meta": {
            "date": run_at,
            "transport": args.transport,
            "duration_s": args.duration,
            "warmup_s": args.warmup,
            "host": host,
            "fw_version": status_pre.get("fw_version"),
            "proto_version": status_pre.get("proto_version"),
            "config": {k: status_pre.get(k) for k in
                       ("resolution", "quality", "xclk_mhz", "fb_count")},
            "rssi_dbm": status_pre.get("rssi"),
            "pc": {
                "cpu": platform.processor(),
                "os": platform.platform(),
                "python": platform.python_version(),
            },
        },
        "device_status_pre": status_pre,
        "device_status_post": status_post,
        "cpu": CpuMeter.deltas(cpu_a, cpu_b),
        "sync": {"offset_us": offset_us,
                 "rtt_ms": [round(r, 2) for r in rtts]},
        "results": report,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2))
    print(json.dumps(report, indent=2))
    print(f"written {out}")


if __name__ == "__main__":
    main()
