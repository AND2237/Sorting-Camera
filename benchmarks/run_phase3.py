#!/usr/bin/env python3
"""Phase 3 orchestrator: HTTP MJPEG vs TCP framed vs UDP packetized.

Runs every transport against Config A (VGA/q12) and Config B (HD/q12)
per docs/benchmark-plan.md (10 s warm-up, 120 s run), writes raw JSON
per run plus an aggregate summary. All numbers come from the runs.
"""

import argparse
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH = HERE / "transport_bench.py"

CONFIGS = {
    "A": {"framesize": "vga", "quality": 12},
    "B": {"framesize": "hd", "quality": 12},
}


def get_status(host, timeout=4):
    with urllib.request.urlopen(f"http://{host}:80/api/v1/status", timeout=timeout) as r:
        return json.loads(r.read().decode())


def set_config(host, params, timeout=4):
    # POST with a JSON body since FW-13: the camera refuses a write that
    # arrives as a query string, and a quality of 12 has to reach it as the
    # number 12 rather than the string "12".
    request = urllib.request.Request(
        f"http://{host}:80/api/v1/config",
        data=json.dumps(params).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return json.loads(r.read().decode())


def wait_idle(host, tries=8):
    for _ in range(tries):
        s = get_status(host)
        if not (s.get("stream_clients") or s.get("tcp_clients")):
            return s
        time.sleep(1.0)
    raise RuntimeError("device busy: stream/tcp client still connected")


def pretty(row):
    r = row["results"]
    lat = r.get("latency_ms") or {}
    fps = r.get("fps") or {}
    cpu = row.get("cpu") or {}
    drops = r.get("drops") or {}
    total_drops = sum(v for v in drops.values() if isinstance(v, int))
    return (f"| {row['meta']['config'].get('resolution')} | {row['meta']['transport']} "
            f"| {fps.get('per_second_median')} | {fps.get('per_second_p95')} "
            f"| {(r.get('interval_ms') or {}).get('p95')} "
            f"| {total_drops} | {(lat or {}).get('p50')} | {(lat or {}).get('p95')} "
            f"| {r.get('throughput_mbps')} | {cpu.get('process_cpu_pct_onecore')} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--duration", type=int, default=120)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--configs", default="AB", help="subset of A,B")
    ap.add_argument("--transports", default="http,tcp,udp")
    ap.add_argument("--out-dir", default=str(HERE / "results"))
    args = ap.parse_args()

    transports = [t.strip() for t in args.transports.split(",") if t.strip()]
    configs = [c for c in args.configs.upper() if c in CONFIGS]
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    rows = []

    print("| res | transport | fps p50 | fps p95 | int p95 ms | drops | lat p50 | lat p95 | Mbps | cpu% |")
    print("|---|---|---|---|---|---|---|---|---|---|")

    for cfg in configs:
        wait_idle(args.host)
        applied = set_config(args.host, CONFIGS[cfg])
        print(f"[cfg {cfg}] applied: {applied}")
        time.sleep(3.0)

        for transport in transports:
            s = wait_idle(args.host)
            if s.get("quality") != int(CONFIGS[cfg]["quality"]):
                raise RuntimeError(f"config drift: quality={s.get('quality')}")
            tag = f"phase3-{stamp}-{cfg}-{transport}"
            out = out_dir / f"{tag}.json"
            cmd = [sys.executable, str(BENCH), "--transport", transport,
                   "--host", args.host, "--duration", str(args.duration),
                   "--warmup", str(args.warmup), "--out", str(out)]
            print(f"[run] cfg {cfg} / {transport}")
            proc = subprocess.run(cmd)
            if proc.returncode != 0:
                print(f"[run] FAILED cfg {cfg} {transport} rc={proc.returncode}")
                continue
            row = json.loads(out.read_text())
            rows.append(row)
            print(pretty(row))
            time.sleep(3.0)

    agg = {"meta": {"date": stamp, "duration_s": args.duration,
                    "warmup_s": args.warmup, "host": args.host},
           "runs": rows}
    agg_path = out_dir / f"phase3-{stamp}-aggregate.json"
    agg_path.write_text(json.dumps(agg, indent=2))
    print(f"aggregate written: {agg_path}")


if __name__ == "__main__":
    main()
