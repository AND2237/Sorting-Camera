#!/usr/bin/env python3
"""Phase 2 link test, one command per firmware (docs/performance-workplan.md).

Runs the whole matrix against ONE flashed bench firmware and writes a summary,
so it can be started by hand while the PC is joined to the camera's Wi-Fi (no
internet, no coding agent, no serial monitor needed):

    python benchmarks/phase2_link.py --label baseline --expect baseline   # firmware NBB
    python benchmarks/phase2_link.py --label block    --expect new        # firmware NB

--expect is a safeguard: the device reports the sdkconfig it was built with
(INFO), and the run aborts if that does not match what you said you flashed
(new = 240 MHz / 80m flash / 32768 TCP window, baseline = 160 / 40m / 5760).

Matrix (default): int @8192, int @1460, int @32768, psram @8192, each --reps times
for --seconds. A run counts only if net_bench marks it VALID (no reboot, PC and
device rates within 5%, run counter consistent); an INVALID run is retried up to
--retries more times. Raw per-run JSON goes to benchmarks/results/ via
net_bench's own format; the summary is written next to it as
phase2-link-<label>-<timestamp>.md and .json.

The summary reports measurements only. It draws no conclusion.
"""

import argparse
import json
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_bench as nb  # noqa: E402

EXPECT = {
    "new": {"cpu_mhz": 240, "flash_freq": "80m", "tcp_wnd": 32768},
    "baseline": {"cpu_mhz": 160, "flash_freq": "40m", "tcp_wnd": 5760},
    "any": {},  # no build check: for experiment variants (different buffers, core placement)
}

CELLS = [("int", 8192), ("int", 1460), ("int", 32768), ("psram", 8192)]


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fmt(x, nd=2):
    return "-" if x is None else f"{x:.{nd}f}"


def run_one(args, mode, chunk):
    ns = SimpleNamespace(host=args.host, port=args.port, mode=mode, seconds=args.seconds,
                         chunk=chunk, warmup=args.warmup, label=args.label,
                         connect_timeout=5.0, recv_timeout=8.0)
    try:
        return nb.run(ns), None
    except SystemExit as exc:
        return None, str(exc)


def save_raw(out_dir, label, mode, chunk, result):
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    path = out_dir / f"net-{label}-{mode}-{chunk}-{stamp}.json"
    path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=nb.HOST_DEFAULT)
    ap.add_argument("--port", type=int, default=nb.PORT_DEFAULT)
    ap.add_argument("--label", required=True, help="e.g. baseline or block")
    ap.add_argument("--expect", choices=sorted(EXPECT), required=True,
                    help="which build you believe is flashed: new or baseline")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--retries", type=int, default=2, help="extra attempts per repetition")
    ap.add_argument("--pause", type=float, default=5.0, help="seconds between runs")
    ap.add_argument("--out-dir", default=str(Path(__file__).resolve().parent / "results"))
    args = ap.parse_args()

    info, err = nb.query_info(args.host, args.port)
    if info is None:
        raise SystemExit(f"cannot read device INFO: {err}. Is the PC joined to the camera's Wi-Fi "
                         "and the bench firmware flashed (and GPIO0 released, board reset)?")
    nb.print_info(info, "device before the matrix")

    build = info.get("build", {})
    wrong = [f"{k}: device {build.get(k)!r}, expected {v!r}"
             for k, v in EXPECT[args.expect].items() if build.get(k) != v]
    if wrong:
        raise SystemExit("ABORT: the flashed firmware is not the '" + args.expect + "' build -> "
                         + "; ".join(wrong))
    if info.get("camera_enabled"):
        print("NOTE: this build has the camera enabled; Phase 2 expects the no-camera variants.")
    flash_io = info.get("runtime", {}).get("flash_io")
    print(f"flash read mode at run time: {flash_io}  "
          f"({'QIO active' if flash_io == 'qio' else 'QIO is NOT active'})")

    out_dir = Path(args.out_dir)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    cells_out = []
    total_runs = len(CELLS) * args.reps
    done = 0
    for mode, chunk in CELLS:
        reps_ok, attempts_log = [], []
        for rep in range(1, args.reps + 1):
            got = None
            for attempt in range(1, args.retries + 2):
                done_label = f"[{done + 1}/{total_runs}] {mode} chunk={chunk} rep={rep} try={attempt}"
                print(f"\n{done_label}  ({args.seconds} s)")
                result, rerr = run_one(args, mode, chunk)
                if result is None:
                    print(f"   FAILED: {rerr}")
                    attempts_log.append({"rep": rep, "attempt": attempt, "error": rerr})
                    time.sleep(args.pause)
                    continue
                path = save_raw(out_dir, args.label, mode, chunk, result)
                ps = result["received"]["per_second_mbps"]
                print(f"   {'VALID' if result['valid'] else 'INVALID'}  mean "
                      f"{result['received']['mean_mbps_whole_run']:.2f} Mbps  p50 "
                      f"{fmt(ps.get('p50'))}  -> {path.name}")
                for why in result.get("invalid_reasons", []):
                    print(f"      - {why}")
                attempts_log.append({"rep": rep, "attempt": attempt, "valid": result["valid"],
                                     "file": path.name,
                                     "reasons": result.get("invalid_reasons", [])})
                time.sleep(args.pause)
                if result["valid"]:
                    got = result
                    break
            done += 1
            if got is not None:
                reps_ok.append(got)

        def pick(f):
            return [f(r) for r in reps_ok]

        last = lambda r: (r.get("device", {}).get("after") or {}).get("last", {})  # noqa: E731
        cells_out.append({
            "mode": mode, "chunk": chunk, "valid_reps": len(reps_ok), "reps_wanted": args.reps,
            "pc_mean_mbps_median": med(pick(lambda r: r["received"]["mean_mbps_whole_run"])),
            "pc_mean_mbps_all": pick(lambda r: round(r["received"]["mean_mbps_whole_run"], 3)),
            "per_second_p5_median": med(pick(lambda r: r["received"]["per_second_mbps"].get("p5"))),
            "per_second_p50_median": med(pick(lambda r: r["received"]["per_second_mbps"].get("p50"))),
            "per_second_p95_median": med(pick(lambda r: r["received"]["per_second_mbps"].get("p95"))),
            "stalled_seconds_total": sum(pick(lambda r: r["received"]["stalled_seconds_below_half_median"])),
            "device_mbps_median": med(pick(lambda r: last(r).get("mbps"))),
            "slow_sends_total": sum(pick(lambda r: last(r).get("slow_sends") or 0)),
            "max_send_ms_max": max(pick(lambda r: last(r).get("max_send_ms") or 0), default=None),
            "min_free_internal_min": min([x for x in pick(lambda r: last(r).get("min_free_internal"))
                                          if x], default=None),
            "attempts": attempts_log,
        })

    final_info, ferr = nb.query_info(args.host, args.port)

    lines = [f"# Phase 2 link test - {args.label} (expected build: {args.expect})", "",
             f"_Run {datetime.now().isoformat(timespec='seconds')}; {args.seconds} s per run, "
             f"warm-up {args.warmup} s, {args.reps} repetitions per cell, retries {args.retries}. "
             "Measurements only; no conclusion drawn._", "",
             "## Device (INFO before the matrix)", "", "```json", json.dumps(info, indent=2), "```", "",
             "## Results (median over valid repetitions)", "",
             "| Cell | valid reps | PC mean Mbps (median) | all reps | per-second p5 / p50 / p95 | "
             "stalled s | device Mbps | slow_sends | max_send ms | min free internal |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for c in cells_out:
        lines.append(
            f"| {c['mode']} @{c['chunk']} | {c['valid_reps']}/{c['reps_wanted']} | "
            f"{fmt(c['pc_mean_mbps_median'])} | {c['pc_mean_mbps_all']} | "
            f"{fmt(c['per_second_p5_median'])} / {fmt(c['per_second_p50_median'])} / "
            f"{fmt(c['per_second_p95_median'])} | {c['stalled_seconds_total']} | "
            f"{fmt(c['device_mbps_median'])} | {c['slow_sends_total']} | "
            f"{fmt(c['max_send_ms_max'], 1)} | {c['min_free_internal_min']} |")
    bad = [(c["mode"], c["chunk"], a) for c in cells_out for a in c["attempts"]
           if a.get("valid") is False or "error" in a]
    lines += ["", "## Invalid or failed attempts", ""]
    if bad:
        for mode, chunk, a in bad:
            lines.append(f"- {mode} @{chunk} rep {a['rep']} try {a['attempt']}: "
                         f"{a.get('error') or '; '.join(a.get('reasons', []))}")
    else:
        lines.append("- none")
    if final_info:
        rr = final_info.get("reset_reason")
        lines += ["", "## Device after the matrix", "",
                  f"- reset_reason {nb.RESET_REASON_NAMES.get(rr, rr)}, uptime {final_info.get('uptime_s')} s",
                  f"- min free internal RAM ever: {final_info.get('runtime', {}).get('min_free_internal')}"]
    else:
        lines += ["", "## Device after the matrix", "", f"- INFO unavailable: {ferr}"]

    md = out_dir / f"phase2-link-{args.label}-{stamp}.md"
    js = out_dir / f"phase2-link-{args.label}-{stamp}.json"
    md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    js.write_text(json.dumps({"label": args.label, "expect": args.expect, "info_before": info,
                              "info_after": final_info, "cells": cells_out}, indent=2),
                  encoding="utf-8")
    print("\n" + "\n".join(lines[lines.index("## Results (median over valid repetitions)"):]))
    print(f"\nsaved {md}\nsaved {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
