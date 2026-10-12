#!/usr/bin/env python3
"""Phase 2, camera ON: does the tuning block change fps or RAM headroom with real frames?

One command per flashed bench-with-camera firmware (NC = new block, NCB = baseline).
It sets each camera profile over the device's control API, then streams real JPEG
frames over the bench port (net_bench mode "cam": no HTTP server in the path) and
records fps, frame size, link rate, stalls and internal-RAM headroom:

    python benchmarks/phase2_cam.py --label baseline --expect baseline   # NCB
    python benchmarks/phase2_cam.py --label block    --expect new        # NC
    python benchmarks/phase2_cam.py --label baseline2 --expect baseline  # NCB again (drift check)

Run it with the SAME scene, lamp and camera position for every firmware; frame
size, and so fps, follows the scene (docs/performance-workplan.md, F8).

Profiles (default): svga:12 (control), xga:24, sxga:24, uxga:36, uxga:16, at
--xclk 18 MHz. Custom: --profiles svga:12,xga:24:20  (framesize:quality[:xclk]).
The firmware REFUSES a quality below the measured safe floor of a resolution (camera.c
s_size_models: svga 2, xga 6, hd 6, sxga 8, uxga 16), so uxga:16 is the best UXGA quality
it will accept; a refused profile is reported as FAILED, never silently changed.

A repetition counts only if: net_bench marks it VALID (no reboot, PC and device rates
within 5%), frames arrived and parsed, the device reported no camera failures, the
camera-recovery counter did not move, and the resolution/quality/xclk the device
reports are the ones requested. Invalid ones are retried (--retries).

The control password comes from --password, SCAM_CONTROL_PASSWORD, or the git-ignored
config_secrets.h (as phase5_pipeline.py does). It is never written to any output file.
The summary reports measurements only; it draws no conclusion.
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
import phase5_pipeline as p5  # noqa: E402

EXPECT = {
    "new": {"cpu_mhz": 240, "flash_freq": "80m", "tcp_wnd": 32768},
    "baseline": {"cpu_mhz": 160, "flash_freq": "40m", "tcp_wnd": 5760},
}
DEFAULT_PROFILES = "svga:12,xga:24,sxga:24,uxga:36,uxga:16"
RES = {"qqvga": "160x120", "qvga": "320x240", "vga": "640x480", "svga": "800x600",
       "xga": "1024x768", "hd": "1280x720", "sxga": "1280x1024", "uxga": "1600x1200"}
NAME_OF = {v: k for k, v in RES.items()}


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fmt(x, nd=2):
    return "-" if x is None else f"{x:.{nd}f}"


class Control:
    """Minimal authenticated client for the device's HTTP control API."""

    def __init__(self, host, port, password):
        self.host, self.port, self.password = host, port, password
        self.token = None
        self.probe_error = None

    def _token(self, force=False):
        if self.token and not force:
            return self.token
        self.token = p5.authenticate(self.host, self.password, self.port) if self.password else None
        return self.token

    def call(self, method, path, body=None, timeout=12.0):
        status, obj, text = 0, None, ""
        for attempt in (1, 2):
            status, obj, text = p5.raw_request(self.host, path, self.port, timeout, method, body,
                                               self._token(force=(attempt == 2)))
            if status != 401:
                break
        return status, obj, text

    def auth_required(self):
        status, obj, text = p5.raw_request(self.host, "/api/v1/capabilities", self.port, 6.0)
        if status == 401:
            return True
        if status == 200:
            return bool(obj and obj.get("auth", {}).get("required"))
        self.probe_error = text.strip() or ("HTTP %d" % status if status else "no response")
        return None


def parse_profiles(spec, default_xclk):
    out = []
    for part in spec.split(","):
        bits = part.strip().split(":")
        if len(bits) not in (2, 3) or bits[0] not in RES:
            raise SystemExit(f"bad profile '{part}': use framesize:quality[:xclk], framesize in "
                             + "|".join(RES))
        out.append((bits[0], int(bits[1]), int(bits[2]) if len(bits) == 3 else default_xclk))
    return out


def device_state(ctl):
    """(resolution name, quality, xclk_mhz, recoveries, capture_failures) or None."""
    s1, cfg, _ = ctl.call("GET", "/api/v1/config")
    s2, st, _ = ctl.call("GET", "/api/v1/status")
    if s1 != 200 or not cfg:
        return None
    return {"fs": NAME_OF.get(cfg.get("resolution"), cfg.get("resolution")),
            "quality": cfg.get("quality"), "xclk": cfg.get("xclk_mhz"),
            "recoveries": cfg.get("camera_recoveries", (st or {}).get("camera_recoveries", 0)),
            "capture_failures": (st or {}).get("capture_failures", 0)}


def apply_profile(ctl, fs, quality, xclk, settle):
    for _ in range(3):
        status, obj, text = ctl.call("POST", "/api/v1/config",
                                     {"framesize": fs, "quality": quality, "xclk": xclk}, 25.0)
        if status == 200:
            break
        time.sleep(2.0)
    else:
        return None, f"config POST failed (HTTP {status}): {text[:120]}"
    time.sleep(settle)
    st = device_state(ctl)
    if st is None:
        return None, "cannot read config back"
    return st, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=nb.HOST_DEFAULT)
    ap.add_argument("--port", type=int, default=nb.PORT_DEFAULT, help="bench port")
    ap.add_argument("--control-port", type=int, default=80)
    ap.add_argument("--label", required=True)
    ap.add_argument("--expect", choices=sorted(EXPECT), required=True)
    ap.add_argument("--profiles", default=DEFAULT_PROFILES)
    ap.add_argument("--xclk", type=int, default=18)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--retries", type=int, default=2)
    ap.add_argument("--settle", type=float, default=4.0, help="seconds after a config change")
    ap.add_argument("--pause", type=float, default=4.0)
    ap.add_argument("--password", default=None)
    ap.add_argument("--no-auth", action="store_true")
    ap.add_argument("--out-dir", default=str(Path(__file__).resolve().parent / "results"))
    args = ap.parse_args()

    profiles = parse_profiles(args.profiles, args.xclk)

    info, err = nb.query_info(args.host, args.port)
    if info is None:
        raise SystemExit(f"cannot read device INFO: {err}. Joined to the camera's Wi-Fi? Bench "
                         "firmware flashed?")
    nb.print_info(info, "device before the matrix")
    build = info.get("build", {})
    wrong = [f"{k}: device {build.get(k)!r}, expected {v!r}"
             for k, v in EXPECT[args.expect].items() if build.get(k) != v]
    if wrong:
        raise SystemExit("ABORT: the flashed firmware is not the '" + args.expect + "' build -> "
                         + "; ".join(wrong))
    if not info.get("camera_enabled"):
        raise SystemExit("ABORT: this bench build has the camera DISABLED. Flash the camera-on "
                         "variant (NC = new block, NCB = baseline).")

    password = None if args.no_auth else p5.read_password(args.password)
    ctl = Control(args.host, args.control_port, password)
    required = ctl.auth_required()
    if required is None:
        raise SystemExit("cannot reach the control API on port %d: %s"
                         % (args.control_port, ctl.probe_error or "connection failed"))
    if required and not password:
        raise SystemExit("device requires a control password: pass --password or set "
                         "SCAM_CONTROL_PASSWORD")
    original = device_state(ctl)
    if original is None:
        raise SystemExit("control API reachable but config/status unreadable (authentication?)")
    print(f"control API ok (auth required={required}); current camera state: {original}")

    out_dir = Path(args.out_dir)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    results = []
    for fs, quality, xclk in profiles:
        print(f"\n=== profile {fs} q{quality} @ {xclk} MHz ===")
        reps_ok, attempts_log = [], []
        for rep in range(1, args.reps + 1):
            got = None
            for attempt in range(1, args.retries + 2):
                print(f"\n[{fs} q{quality}] rep {rep} try {attempt} ({args.seconds} s)")
                applied, aerr = apply_profile(ctl, fs, quality, xclk, args.settle)
                if applied is None:
                    print(f"   FAILED: {aerr}")
                    attempts_log.append({"rep": rep, "attempt": attempt, "error": aerr})
                    time.sleep(args.pause)
                    continue
                problems = []
                if applied["fs"] != fs or applied["quality"] != quality or applied["xclk"] != xclk:
                    problems.append(f"device reports {applied['fs']} q{applied['quality']} "
                                    f"@{applied['xclk']} MHz, not the requested "
                                    f"{fs} q{quality} @{xclk} MHz")
                before = device_state(ctl) or applied
                ns = SimpleNamespace(host=args.host, port=args.port, mode="cam",
                                     seconds=args.seconds, chunk=8192, warmup=args.warmup,
                                     label=args.label, connect_timeout=5.0, recv_timeout=10.0,
                                     sample_path=(out_dir / f"sample-{args.label}-{fs}-q{quality}.jpg")
                                     if rep == 1 else None)
                try:
                    r = nb.run(ns)
                except SystemExit as exc:
                    print(f"   FAILED: {exc}")
                    attempts_log.append({"rep": rep, "attempt": attempt, "error": str(exc)})
                    time.sleep(args.pause)
                    continue
                after = device_state(ctl) or before
                problems += r.get("invalid_reasons", [])
                fr = r.get("frames", {})
                if not fr.get("count"):
                    problems.append("no frames received")
                if fr.get("protocol_errors") or fr.get("desynchronised"):
                    problems.append("frame stream desynchronised")
                jp = fr.get("jpeg") or {}
                if jp.get("frames_checked"):
                    if jp.get("dominant_dims") != RES[fs]:
                        problems.append(f"frames are really {jp.get('dims')}, not the requested "
                                        f"{RES[fs]} - the sensor is not producing the requested mode")
                    if (jp.get("eoi_ok_pct") or 0) < 99.0:
                        problems.append(f"only {jp.get('eoi_ok_pct'):.1f}% of frames end with the "
                                        "JPEG end marker (truncated frames)")
                last = (r.get("device", {}).get("after") or {}).get("last", {})
                if last.get("cam_fail"):
                    problems.append(f"device reported {last['cam_fail']} camera failures")
                rec = (after.get("recoveries") or 0) - (before.get("recoveries") or 0)
                if rec:
                    problems.append(f"camera recoveries moved by {rec} during the run")
                if (after["fs"], after["quality"], after["xclk"]) != (before["fs"], before["quality"], before["xclk"]):
                    problems.append("camera configuration changed during the run")
                r["profile"] = {"framesize": fs, "quality": quality, "xclk": xclk}
                r["valid"] = not problems
                r["invalid_reasons"] = problems
                out_dir.mkdir(parents=True, exist_ok=True)
                raw = out_dir / f"cam-{args.label}-{fs}-q{quality}-{datetime.now().strftime('%Y%m%d-%H%M%S-%f')}.json"
                raw.write_text(json.dumps(r, indent=2), encoding="utf-8")
                fb = fr.get("frame_bytes", {})
                print(f"   actual frame size {jp.get('dims')}  end-marker ok {fmt(jp.get('eoi_ok_pct'), 1)}%")
                print(f"   {'VALID' if not problems else 'INVALID'}  fps {fmt(fr.get('fps_after_warmup'))}  "
                      f"frame p50 {fmt((fb.get('p50') or 0) / 1024, 1)} KiB  link "
                      f"{r['received']['mean_mbps_whole_run']:.2f} Mbps  -> {raw.name}")
                for why in problems:
                    print(f"      - {why}")
                attempts_log.append({"rep": rep, "attempt": attempt, "valid": not problems,
                                     "file": raw.name, "reasons": problems})
                time.sleep(args.pause)
                if not problems:
                    got = r
                    break
            if got is not None:
                reps_ok.append(got)

        def pick(f):
            return [f(r) for r in reps_ok]

        lastf = lambda r: (r.get("device", {}).get("after") or {}).get("last", {})  # noqa: E731
        results.append({
            "profile": f"{fs} q{quality} @{xclk}", "framesize": fs, "quality": quality, "xclk": xclk,
            "valid_reps": len(reps_ok), "reps_wanted": args.reps,
            "fps_median": med(pick(lambda r: r["frames"]["fps_after_warmup"])),
            "fps_all": pick(lambda r: round(r["frames"]["fps_after_warmup"] or 0, 2)),
            "device_fps_median": med(pick(lambda r: lastf(r).get("fps"))),
            "frame_bytes_p50": med(pick(lambda r: r["frames"]["frame_bytes"].get("p50"))),
            "frame_bytes_p95": med(pick(lambda r: r["frames"]["frame_bytes"].get("p95"))),
            "interval_ms_p95": med(pick(lambda r: r["frames"]["interval_ms"].get("p95"))),
            "actual_dims": [r["frames"].get("jpeg", {}).get("dominant_dims") for r in reps_ok][:1] or [None],
            "eoi_ok_pct_min": min(pick(lambda r: r["frames"].get("jpeg", {}).get("eoi_ok_pct") or 0), default=None),
            "sample": next((r["frames"]["jpeg"].get("sample") for r in reps_ok
                            if r["frames"].get("jpeg", {}).get("sample")), None),
            "link_mbps_median": med(pick(lambda r: r["received"]["mean_mbps_whole_run"])),
            "stalled_seconds_total": sum(pick(lambda r: r["received"]["stalled_seconds_below_half_median"])),
            "slow_sends_total": sum(pick(lambda r: lastf(r).get("slow_sends") or 0)),
            "min_free_internal_min": min([x for x in pick(lambda r: lastf(r).get("min_free_internal"))
                                          if x], default=None),
            "attempts": attempts_log,
        })

    if original:
        restore_fs = original["fs"] if original["fs"] in RES else None
        if restore_fs:
            ctl.call("POST", "/api/v1/config", {"framesize": restore_fs, "quality": original["quality"],
                                                "xclk": original["xclk"]}, 25.0)
    final_info, ferr = nb.query_info(args.host, args.port)

    lines = [f"# Phase 2 camera-on link test - {args.label} (expected build: {args.expect})", "",
             f"_Run {datetime.now().isoformat(timespec='seconds')}; {args.seconds} s per run, warm-up "
             f"{args.warmup} s, {args.reps} repetitions per profile, retries {args.retries}. Real camera "
             "frames over the bench port (no HTTP server in the path). Same scene required for every "
             "firmware compared. Measurements only; no conclusion drawn._", "",
             "## Device (INFO before the matrix)", "", "```json", json.dumps(info, indent=2), "```", "",
             "## Results (median over valid repetitions)", "",
             "| Profile | valid reps | fps (PC) | all reps | fps (device) | frame KiB p50 / p95 | "
             "frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal | "
             "actual WxH | end-marker ok % |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for c in results:
        kb = lambda v: "-" if v is None else f"{v / 1024:.1f}"  # noqa: E731
        lines.append(f"| {c['profile']} | {c['valid_reps']}/{c['reps_wanted']} | {fmt(c['fps_median'])} | "
                     f"{c['fps_all']} | {fmt(c['device_fps_median'])} | {kb(c['frame_bytes_p50'])} / "
                     f"{kb(c['frame_bytes_p95'])} | {fmt(c['interval_ms_p95'], 1)} | "
                     f"{fmt(c['link_mbps_median'])} | {c['stalled_seconds_total']} | "
                     f"{c['slow_sends_total']} | {c['min_free_internal_min']} | "
                     f"{c['actual_dims'][0]} | {fmt(c['eoi_ok_pct_min'], 1)} |")
    bad = [(c["profile"], a) for c in results for a in c["attempts"]
           if a.get("valid") is False or "error" in a]
    lines += ["", "## Invalid or failed attempts", ""]
    if bad:
        for prof, a in bad:
            lines.append(f"- {prof} rep {a['rep']} try {a['attempt']}: "
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

    md = out_dir / f"phase2-cam-{args.label}-{stamp}.md"
    js = out_dir / f"phase2-cam-{args.label}-{stamp}.json"
    md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    js.write_text(json.dumps({"label": args.label, "expect": args.expect, "info_before": info,
                              "info_after": final_info, "profiles": results}, indent=2),
                  encoding="utf-8")
    print("\n" + "\n".join(lines[lines.index("## Results (median over valid repetitions)"):]))
    print(f"\nsaved {md}\nsaved {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
