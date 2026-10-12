#!/usr/bin/env python3
"""Exposure / gain sweep: how much of the frame-rate limit is LIGHT?

With auto exposure the sensor lengthens its exposure when the scene is dim, and a long
exposure lengthens the frame time, so fps falls whatever the Wi-Fi or the frame size.
This streams real frames from the bench-with-camera firmware (NC / NCB) at one profile
under several sensor settings (auto exposure with different gain ceilings, and fixed
manual exposure/gain) and reports fps, longest gap between frames, frame size and mean
picture brightness for each:

    python benchmarks/exposure_sweep.py
    python benchmarks/exposure_sweep.py --profiles svga:12,xga:24 --seconds 10 --reps 2
    python benchmarks/exposure_sweep.py --sets "auto|aec=1,aec2=1,agc=1,gainceiling=0;m400|aec=0,aec2=0,aec_value=400,agc=0,agc_gain=8"

Keep the scene and the lighting fixed during the whole sweep; change the lighting only
between sweeps and name each with --label (e.g. --label room, --label lamp).

Safety: the settings are saved by the device (NVS). The sweep reads the device's sensor
controls first and puts every one of them back at the end (also when it fails midway).
A set the device refuses is reported and skipped, never silently changed.

Brightness is measured only if Pillow is installed (pip install pillow); without it the
sample pictures are saved for you to look at (benchmarks/results/sample-exposure-*.jpg).
The control password comes from --password, SCAM_CONTROL_PASSWORD or config_secrets.h.
"""

import argparse
import io
import json
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_bench as nb  # noqa: E402
import phase2_cam as pc  # noqa: E402
import phase5_pipeline as p5  # noqa: E402

DEFAULT_SETS = [
    ("auto-default", {"aec": 1, "aec2": 1, "agc": 1, "gainceiling": 0, "ae_level": 0}),
    ("auto-gain32x", {"aec": 1, "aec2": 1, "agc": 1, "gainceiling": 4}),
    ("auto-gain128x", {"aec": 1, "aec2": 1, "agc": 1, "gainceiling": 6}),
    ("manual-e300-g0", {"aec": 0, "aec2": 0, "aec_value": 300, "agc": 0, "agc_gain": 0}),
    ("manual-e600-g10", {"aec": 0, "aec2": 0, "aec_value": 600, "agc": 0, "agc_gain": 10}),
    ("manual-e1000-g20", {"aec": 0, "aec2": 0, "aec_value": 1000, "agc": 0, "agc_gain": 20}),
]


def parse_sets(spec):
    out = []
    for item in spec.split(";"):
        name, _, kv = item.partition("|")
        if not name.strip() or not kv.strip():
            raise SystemExit(f"bad set '{item}': use name|control=value,control=value")
        d = {}
        for part in kv.split(","):
            k, _, v = part.partition("=")
            if not k.strip() or not v.strip().lstrip("-").isdigit():
                raise SystemExit(f"bad control '{part}' in set '{name}': use name=integer")
            d[k.strip()] = int(v)
        out.append((name.strip(), d))
    return out


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fmt(x, nd=2):
    return "-" if x is None else f"{x:.{nd}f}"


def mean_luma(path):
    try:
        from PIL import Image, ImageStat  # noqa: PLC0415
        return ImageStat.Stat(Image.open(path).convert("L")).mean[0]
    except Exception:  # Pillow missing or unreadable image
        return None


def post_sensor(ctl, values):
    items = list(values.items())
    for i in range(0, len(items), 8):  # the endpoint accepts at most 512 bytes of JSON
        status, _, text = ctl.call("POST", "/api/v1/sensor", dict(items[i:i + 8]))
        if status != 200:
            return f"HTTP {status}: {text[:140]}"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=nb.HOST_DEFAULT)
    ap.add_argument("--port", type=int, default=nb.PORT_DEFAULT, help="bench port")
    ap.add_argument("--control-port", type=int, default=80)
    ap.add_argument("--label", default="sweep", help="name of the lighting condition")
    ap.add_argument("--profiles", default="xga:24")
    ap.add_argument("--xclk", type=int, default=18)
    ap.add_argument("--sets", default=None, help="name|ctl=v,ctl=v;name2|... (default: six built-in sets)")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--converge", type=float, default=4.0, help="seconds after a sensor change")
    ap.add_argument("--settle", type=float, default=4.0, help="seconds after a profile change")
    ap.add_argument("--pause", type=float, default=2.0)
    ap.add_argument("--password", default=None)
    ap.add_argument("--no-auth", action="store_true")
    ap.add_argument("--out-dir", default=str(Path(__file__).resolve().parent / "results"))
    args = ap.parse_args()

    sets = parse_sets(args.sets) if args.sets else DEFAULT_SETS
    profiles = pc.parse_profiles(args.profiles, args.xclk)

    info, err = nb.query_info(args.host, args.port)
    if info is None:
        raise SystemExit(f"cannot read device INFO: {err}. Joined to the camera's Wi-Fi? Bench-with-camera "
                         "firmware flashed?")
    if not info.get("camera_enabled"):
        raise SystemExit("ABORT: this bench build has the camera disabled (flash NC or NCB).")
    nb.print_info(info, "device before the sweep")

    password = None if args.no_auth else p5.read_password(args.password)
    ctl = pc.Control(args.host, args.control_port, password)
    status, _, _ = p5.raw_request(args.host, "/api/v1/capabilities", args.control_port, 6.0)
    if status == 0:
        raise SystemExit(f"cannot reach the control API on port {args.control_port}")
    if status == 401 and not password:
        raise SystemExit("the device requires a control password: pass --password or set "
                         "SCAM_CONTROL_PASSWORD")
    s, original_sensor, _ = ctl.call("GET", "/api/v1/sensor")
    if s != 200 or not original_sensor:
        raise SystemExit(f"cannot read the sensor controls (HTTP {s}); authentication?")
    original_state = pc.device_state(ctl)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    rows = []
    notes = []

    try:
        for fs, quality, xclk in profiles:
            applied, aerr = pc.apply_profile(ctl, fs, quality, xclk, args.settle)
            if applied is None:
                notes.append(f"{fs} q{quality}: profile not applied ({aerr})")
                print(f"\nprofile {fs} q{quality} FAILED: {aerr}")
                continue
            for name, values in sets:
                print(f"\n=== {fs} q{quality} | set '{name}': {values} ===")
                serr = post_sensor(ctl, values)
                if serr:
                    notes.append(f"{fs} q{quality} set '{name}': refused by the device ({serr})")
                    print(f"   REFUSED: {serr}")
                    continue
                time.sleep(args.converge)
                reps = []
                for rep in range(1, args.reps + 1):
                    sample = out_dir / f"sample-exposure-{args.label}-{fs}-{name}.jpg" if rep == 1 else None
                    ns = SimpleNamespace(host=args.host, port=args.port, mode="cam", seconds=args.seconds,
                                         chunk=8192, warmup=args.warmup, label=args.label,
                                         connect_timeout=5.0, recv_timeout=15.0, sample_path=sample)
                    try:
                        r = nb.run(ns)
                    except SystemExit as exc:
                        print(f"   rep {rep} FAILED: {exc}")
                        notes.append(f"{fs} q{quality} set '{name}' rep {rep}: {exc}")
                        time.sleep(args.pause)
                        continue
                    fr = r.get("frames", {})
                    jp = fr.get("jpeg") or {}
                    ok = r.get("valid") and fr.get("count") and not fr.get("desynchronised") \
                        and jp.get("dominant_dims") == pc.RES[fs] and (jp.get("eoi_ok_pct") or 0) >= 99
                    luma = mean_luma(sample) if sample and Path(sample).exists() else None
                    rec = {"rep": rep, "valid": bool(ok), "reasons": r.get("invalid_reasons", []),
                           "fps": fr.get("fps_after_warmup"), "device_fps": (r.get("device", {}).get("after") or {})
                           .get("last", {}).get("fps"), "frame_kib_p50": (fr.get("frame_bytes", {}).get("p50") or 0) / 1024,
                           "interval_p95": fr.get("interval_ms", {}).get("p95"),
                           "interval_max": fr.get("interval_ms", {}).get("max"),
                           "mbps": r["received"]["mean_mbps_whole_run"], "luma": luma,
                           "dims": jp.get("dominant_dims")}
                    reps.append(rec)
                    print(f"   rep {rep}: {'VALID' if ok else 'INVALID'}  fps {fmt(rec['fps'])}  "
                          f"frame {fmt(rec['frame_kib_p50'], 1)} KiB  longest gap {fmt(rec['interval_max'], 0)} ms  "
                          f"brightness {fmt(luma, 0)}")
                    time.sleep(args.pause)
                good = [x for x in reps if x["valid"]]
                rows.append({"profile": f"{fs} q{quality} @{xclk}", "set": name, "values": values,
                             "valid_reps": len(good), "reps_wanted": args.reps,
                             "fps_median": med([x["fps"] for x in good]),
                             "fps_all": [round(x["fps"] or 0, 2) for x in good],
                             "interval_p95_median": med([x["interval_p95"] for x in good]),
                             "interval_max_max": max([x["interval_max"] or 0 for x in good], default=None),
                             "frame_kib_median": med([x["frame_kib_p50"] for x in good]),
                             "mbps_median": med([x["mbps"] for x in good]),
                             "luma": next((x["luma"] for x in good if x["luma"] is not None), None),
                             "attempts": reps})
    finally:
        err1 = post_sensor(ctl, {k: v for k, v in original_sensor.items()})
        print("\nsensor controls restored to their values before the sweep" if not err1
              else f"\nWARNING: could not restore the sensor controls ({err1}). Run "
                   "sensor_tool.py --info and --set to put them back.")
        if original_state and original_state.get("fs") in pc.RES:
            ctl.call("POST", "/api/v1/config", {"framesize": original_state["fs"],
                                                "quality": original_state["quality"],
                                                "xclk": original_state["xclk"]}, 25.0)
    final_info, ferr = nb.query_info(args.host, args.port)

    lines = [f"# Exposure sweep - {args.label}", "",
             f"_Run {datetime.now().isoformat(timespec='seconds')}; {args.seconds} s per run, warm-up "
             f"{args.warmup} s, {args.reps} repetitions per cell. Real frames over the bench port. The scene "
             "and lighting named by the label were kept fixed. Measurements only._", "",
             "## Device (INFO before the sweep)", "", "```json", json.dumps(info, indent=2), "```", "",
             "## Sensor controls before the sweep (restored afterwards)", "", "```json",
             json.dumps(original_sensor, indent=2), "```", "",
             "## Results (median over valid repetitions)", "",
             "| Profile | Set | valid reps | fps | all reps | longest gap ms | gap p95 ms | frame KiB | "
             "link Mbps | brightness (0-255) |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for c in rows:
        lines.append(f"| {c['profile']} | {c['set']} | {c['valid_reps']}/{c['reps_wanted']} | "
                     f"{fmt(c['fps_median'])} | {c['fps_all']} | {fmt(c['interval_max_max'], 0)} | "
                     f"{fmt(c['interval_p95_median'], 0)} | {fmt(c['frame_kib_median'], 1)} | "
                     f"{fmt(c['mbps_median'])} | {fmt(c['luma'], 0)} |")
    lines += ["", "## Settings in each set", ""]
    for name, values in sets:
        lines.append(f"- **{name}**: {values}")
    lines += ["", "## Notes, refused sets and failed attempts", ""]
    bad = [f"{c['profile']} / {c['set']} rep {a['rep']}: {'; '.join(a['reasons']) or 'invalid frames'}"
           for c in rows for a in c["attempts"] if not a["valid"]]
    for line in notes + bad:
        lines.append(f"- {line}")
    if not (notes or bad):
        lines.append("- none")
    if final_info:
        rr = final_info.get("reset_reason")
        lines += ["", "## Device after the sweep", "",
                  f"- reset_reason {nb.RESET_REASON_NAMES.get(rr, rr)}, uptime {final_info.get('uptime_s')} s"]
    md = out_dir / f"exposure-sweep-{args.label}-{stamp}.md"
    js = out_dir / f"exposure-sweep-{args.label}-{stamp}.json"
    md.write_text("\n".join(lines) + "\n", encoding="utf-8")
    js.write_text(json.dumps({"label": args.label, "info_before": info, "sensor_before": original_sensor,
                              "rows": rows, "notes": notes}, indent=2), encoding="utf-8")
    print("\n" + "\n".join(lines[lines.index("## Results (median over valid repetitions)"):
                               lines.index("## Settings in each set")]))
    print(f"\nsaved {md}\nsaved {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
