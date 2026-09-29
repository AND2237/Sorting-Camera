#!/usr/bin/env python3
"""Phase 4 camera configuration matrix.

Stages:
  envelope  resolution x quality grid at baseline (fb3, latest, psram, xclk 18)
  fbgrab    fb_count x grab x fbloc cross at the envelope's boundary point
  confirm   120 s steady-state runs for the derived ladder candidates
  report    markdown ladder + matrix summary from the JSONL results

Every number is sampled from live sockets/status on this machine; cells that
stall, reboot the device, or fail config are recorded as-is, never hidden.
Results append incrementally to <out>.jsonl so partial runs survive.
"""

import argparse
import json
import platform
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transport_bench import StatsCollector, device_sync, http_get_json, run_http  # noqa: E402

BASELINE = {"fb_count": 3, "grab": "latest", "fbloc": "psram", "xclk_mhz": 18}
RESOLUTIONS = ["uxga", "sxga", "hd", "xga", "svga", "vga", "qvga"]
QUALITIES = [(4, "highest"), (12, "high"), (24, "balanced"), (36, "performance")]
RES_ECHO = {"qqvga": "160x120", "qvga": "320x240", "vga": "640x480",
            "svga": "800x600", "xga": "1024x768", "hd": "1280x720",
            "sxga": "1280x1024", "uxga": "1600x1200"}
GRAB_ECHO = {"latest": "latest", "cont": "when_empty"}
POLL_S = 1.0
STALL_S = 15.0


def cell_key(c):
    return (c["resolution"], c["quality"], c["fb_count"], c["grab"], c["fbloc"])


def cell_label(c):
    return (f"{c['resolution']}/q{c['quality']}/fb{c['fb_count']}/"
            f"{c['grab']}/{c['fbloc']}")


def make_cell(res, qual, fb=None, grab=None, fbloc=None):
    return {"resolution": res, "quality": qual,
            "fb_count": fb or BASELINE["fb_count"],
            "grab": grab or BASELINE["grab"],
            "fbloc": fbloc or BASELINE["fbloc"],
            "xclk_mhz": BASELINE["xclk_mhz"]}


def pixels(res_str):
    try:
        w, h = res_str.split("x")
        return int(w) * int(h)
    except Exception:
        return 0


def cell_pixels(cell):
    return pixels(RES_ECHO.get(cell["resolution"], ""))


class StatusPoller(threading.Thread):
    def __init__(self, host):
        super().__init__(daemon=True)
        self.host = host
        self.samples = []
        self.events = []
        self.stop_evt = threading.Event()
        self._last_captured = None
        self._last_captured_t = None

    def run(self):
        while not self.stop_evt.wait(POLL_S):
            try:
                s = http_get_json(self.host, "/api/v1/status", timeout=3)
            except Exception:
                continue
            now = time.time()
            prev = self.samples[-1][1] if self.samples else None
            self.samples.append((now, s))
            if prev is not None and s.get("uptime_s", 0) < prev.get("uptime_s", -1):
                self.events.append({"type": "reboot", "t": now,
                                    "uptime_prev": prev.get("uptime_s"),
                                    "reset_reason": s.get("reset_reason")})
                self._last_captured = None
            cap = s.get("frames_captured")
            if cap is not None:
                if cap != self._last_captured:
                    self._last_captured = cap
                    self._last_captured_t = now
                elif self._last_captured_t and \
                        now - self._last_captured_t > STALL_S:
                    self.events.append({"type": "stall", "t": now,
                                        "no_capture_for_s": round(now - self._last_captured_t, 1),
                                        "reset_reason": s.get("reset_reason")})
                    self._last_captured_t = now + 3600

    def finish(self):
        self.stop_evt.set()
        self.join(timeout=5)


def wait_idle(host, timeout=12.0):
    t_end = time.time() + timeout
    while time.time() < t_end:
        try:
            s = http_get_json(host, "/api/v1/status", timeout=3)
        except Exception:
            time.sleep(0.5)
            continue
        if not s.get("stream_clients") and not s.get("tcp_clients") and \
                not s.get("udp_peer"):
            return s
        time.sleep(0.5)
    return None


def wait_device(host, timeout=60.0):
    t_end = time.time() + timeout
    while time.time() < t_end:
        try:
            s = http_get_json(host, "/api/v1/status", timeout=3)
            if s.get("uptime_s", 0) >= 3 and not s.get("stream_clients"):
                return s
        except Exception:
            pass
        time.sleep(1.0)
    return None


def apply_config(host, cell):
    # POST with a JSON body since FW-13: /api/v1/config no longer accepts a
    # write in the query string, and every value here keeps its JSON type.
    request = urllib.request.Request(
        f"http://{host}:80/api/v1/config",
        data=json.dumps({
            "framesize": cell["resolution"],
            "quality": cell["quality"],
            "fb_count": cell["fb_count"],
            "grab": cell["grab"],
            "fbloc": cell["fbloc"],
            "xclk": cell["xclk_mhz"],
        }).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(request, timeout=40) as r:
            body = r.read().decode()
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}: {e.read().decode(errors='replace')[:200]}", None
    except Exception as e:
        return False, f"{type(e).__name__}: {e}", None
    try:
        echo = json.loads(body)
    except Exception:
        return False, f"non-json body: {body[:120]}", None
    want = {"resolution": RES_ECHO[cell["resolution"]],
            "quality": cell["quality"],
            "fb_count": cell["fb_count"],
            "grab_mode": GRAB_ECHO[cell["grab"]],
            "fb_location": cell["fbloc"]}
    mismatch = {k: (v, echo.get(k)) for k, v in want.items() if echo.get(k) != v}
    if mismatch:
        return False, f"echo mismatch {mismatch}", echo
    return True, "", echo


def device_window_stats(poller, t0, warmup):
    win = [(t, s) for t, s in poller.samples if t >= t0 + warmup]
    out = {"capture_fps": None, "delivered_fps": None,
           "capture_failures_delta": None, "uptime_reset": False,
           "reset_reason_end": None, "avg_capture_ms_end": None,
           "heap_end": None, "samples": len(win)}
    if len(win) >= 2:
        (ta, sa), (tb, sb) = win[0], win[-1]
        dt = tb - ta
        if dt >= 1.0:
            dcap = sb["frames_captured"] - sa["frames_captured"]
            ddel = sb["frames_delivered"] - sa["frames_delivered"]
            out["capture_fps"] = round(dcap / dt, 2) if dcap >= 0 else None
            out["delivered_fps"] = round(ddel / dt, 2) if ddel >= 0 else None
            out["capture_failures_delta"] = max(0, sb["capture_failures"] - sa["capture_failures"])
        out["reset_reason_end"] = sb.get("reset_reason")
        out["avg_capture_ms_end"] = sb.get("avg_capture_ms")
        out["heap_end"] = sb.get("free_heap")
    if poller.events:
        out["uptime_reset"] = any(e["type"] == "reboot" for e in poller.events)
    return out


def run_cell(host, cell, duration, warmup, run_index, meta):
    if wait_idle(host) is None:
        return {"cell": cell, "ok": False, "error": "stream client busy (close the app?)",
                "meta": meta, "index": run_index}
    ok, err, echo = apply_config(host, cell)
    if not ok:
        return {"cell": cell, "ok": False, "error": f"config failed: {err}",
                "meta": meta, "index": run_index}
    dev = wait_device(host)
    if dev is None:
        return {"cell": cell, "ok": False, "error": "device unreachable after config",
                "meta": meta, "index": run_index}
    offset_us, _ = device_sync(host, samples=3)
    poller = StatusPoller(host)
    stats = StatsCollector(time.time() * 1e6 + warmup * 1e6)
    poller.start()
    t0 = time.time()
    stop_flag = {"stop": False}
    try:
        run_http(host, duration, stats, stop_flag)
    finally:
        actual = time.time() - t0
        poller.finish()
    time.sleep(0.5)
    try:
        status_post = http_get_json(host, "/api/v1/status", timeout=4)
    except Exception:
        status_post = None
    report = stats.report(max(actual, warmup + 1), warmup, offset_us)
    devw = device_window_stats(poller, t0, warmup)
    events = poller.events
    dead = devw["uptime_reset"] or any(e["type"] == "stall" for e in events) \
        or report["frames_ok"] < 5
    meta = dict(meta)
    meta["fw_version"] = (dev or {}).get("fw_version")
    return {"cell": cell, "ok": True, "error": None, "meta": meta,
            "index": run_index,
            "config_echo": echo,
            "duration_actual_s": round(actual, 1),
            "rx": report,
            "device": devw,
            "events": events,
            "status_pre": dev,
            "status_post": status_post,
            "dead": dead}


def envelope_cells():
    return [make_cell(r, q) for r in RESOLUTIONS for q, _ in QUALITIES]


def boundary_cell(envelope_runs):
    ok = [r for r in envelope_runs if r.get("ok") and not r.get("dead")
          and (r["rx"]["fps"]["mean"] or 0) >= 15]
    if not ok:
        return None
    return max(ok, key=lambda r: (cell_pixels(r["cell"]),
                                  -r["cell"]["quality"]))["cell"]


def fbgrab_cells(base):
    return [make_cell(base["resolution"], base["quality"], fb, grab, loc)
            for fb in (1, 2, 3)
            for grab in ("latest", "cont")
            for loc in ("psram", "dram")]


def pick_fbgrab_winner(runs):
    ok = [r for r in runs if r.get("ok") and not r.get("dead")
          and (r["rx"]["fps"]["mean"] or 0) >= 15]
    if not ok:
        return None
    return min(ok, key=lambda r: (-r["rx"]["fps"]["mean"],
                                  r["rx"]["interval_ms"]["p95"] or 1e9,
                                  r["cell"]["fb_count"],
                                  r["cell"]["grab"] != "latest",
                                  r["cell"]["fbloc"] != "psram"))["cell"]


def confirm_cells(envelope_runs, params, cap=12):
    ok = sorted([r for r in envelope_runs if r.get("ok") and not r.get("dead")
                 and (r["rx"]["fps"]["mean"] or 0) >= 15],
                key=lambda r: (-cell_pixels(r["cell"]), r["cell"]["quality"]))
    cells = []
    for r in ok:
        cells.append(make_cell(r["cell"]["resolution"], r["cell"]["quality"],
                               params["fb_count"], params["grab"], params["fbloc"]))
    failing = sorted([r for r in envelope_runs if r.get("ok") and not r.get("dead")
                      and (r["rx"]["fps"]["mean"] or 0) < 15],
                     key=lambda r: (-cell_pixels(r["cell"]), r["cell"]["quality"]))
    if failing:
        b = failing[0]["cell"]
        cells.append(make_cell(b["resolution"], b["quality"],
                               params["fb_count"], params["grab"], params["fbloc"]))
    return cells[:cap]


def derive_ladder(runs):
    confirm = [r for r in runs if r.get("meta", {}).get("stage") == "confirm"
               and r.get("ok")]
    eligible = [r for r in confirm if not r.get("dead")
                and (r["rx"]["fps"]["mean"] or 0) >= 15
                and (r["rx"]["fps"]["per_second_median"] or 0) >= 15
                and not r["rx"]["drops"].get("bad")]
    eligible.sort(key=lambda r: (-pixels(RES_ECHO[r["cell"]["resolution"]]),
                                 r["cell"]["quality"]))
    return eligible


def print_report(runs):
    print("\n=== Phase 4 report ===")
    dead = [r for r in runs if r.get("ok") and r.get("dead")]
    errors = [r for r in runs if not r.get("ok")]
    print(f"runs={len(runs)} errors={len(errors)} dead_cells={len(dead)}")
    for r in errors:
        print(f"  ERROR {cell_label(r['cell'])}: {r['error']}")
    for r in dead:
        ev = ",".join(e["type"] for e in r.get("events", [])) or "few_frames"
        print(f"  DEAD  {cell_label(r['cell'])}: {ev} "
              f"(frames_ok={r['rx']['frames_ok']})")

    fbgrab = [r for r in runs if r.get("meta", {}).get("stage") == "fbgrab" and r.get("ok")]
    if fbgrab:
        print("\n--- fb/grab/fbloc cross ---")
        for r in sorted(fbgrab, key=lambda r: -(r["rx"]["fps"]["mean"] or 0)):
            m = r["rx"]["fps"]["mean"]
            print(f"  {cell_label(r['cell']):38s} fps={m} "
                  f"size_p50={r['rx']['frame_bytes']['p50']} dead={r.get('dead')}")

    ladder = derive_ladder(runs)
    print("\n--- operating-point ladder (best quality first) ---")
    print("| # | config | fps mean | fps p50/s | bytes p50 | p95 interval | >=20 |")
    print("|---|---|---|---|---|---|---|")
    for i, r in enumerate(ladder, 1):
        rx = r["rx"]
        meets20 = "yes" if (rx["fps"]["mean"] or 0) >= 20 else "no"
        print(f"| {i} | {cell_label(r['cell'])} | {rx['fps']['mean']} | "
              f"{rx['fps']['per_second_median']} | {rx['frame_bytes']['p50']} | "
              f"{rx['interval_ms']['p95']} ms | {meets20} |")
    if not ladder:
        print("(no confirmed ladder runs yet — run the confirm stage)")
    return ladder


def load_jsonl(path):
    runs = []
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            if isinstance(obj, dict) and "cell" in obj:
                runs.append(obj)
    return runs


def meta_of(args, transport="http"):
    return {"stage": None, "transport": transport, "host": args.host,
            "fw_version": None, "pc": {"cpu": platform.processor(),
                                       "os": platform.platform(),
                                       "python": platform.python_version()}}


def snapshot_ok(host, timeout=6):
    try:
        data = urllib.request.urlopen(
            f"http://{host}/api/v1/snapshot", timeout=timeout).read()
        return data[:2] == b"\xff\xd8" and len(data) > 1024
    except Exception:
        return False


def recover_dead_camera(host):
    """Repair the camera without a reboot when possible: the firmware's
    config endpoint re-runs sensor power-cycle + SCCB bus recovery on
    demand, so a known-good config restores streaming. Falls back to the
    15 s stall-watchdog reboot (stream client attached, no frames)."""
    if snapshot_ok(host):
        print("[recover] camera healthy, no repair needed", flush=True)
        return True
    print("[recover] requesting camera self-repair via config ...", flush=True)
    request = urllib.request.Request(
        f"http://{host}/api/v1/config",
        data=json.dumps({"framesize": "hd", "quality": 12,
                         "fb_count": 3, "grab": "latest",
                         "fbloc": "psram", "xclk": 18}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    for attempt in range(3):
        try:
            urllib.request.urlopen(request, timeout=40).read()
            if snapshot_ok(host):
                print(f"[recover] self-repair ok (attempt {attempt + 1})",
                      flush=True)
                return True
        except Exception as e:
            print(f"[recover] repair attempt {attempt + 1}: {e}", flush=True)
        time.sleep(2)
    print("[recover] self-repair failed, trying stall-watchdog reboot ...",
          flush=True)
    stats = StatsCollector(0)
    stop = {"stop": False}
    try:
        run_http(host, 25, stats, stop)
    except Exception:
        pass
    dev = wait_device(host, timeout=60)
    healthy = dev and snapshot_ok(host)
    print(f"[recover] device {'back up' if dev else 'STILL DOWN'}, "
          f"camera {'healthy' if healthy else 'STILL DEAD'}", flush=True)
    return healthy or None


def execute(args, cells, stage, duration, warmup, out_jsonl, runs):
    consecutive_errors = 0
    for i, cell in enumerate(cells):
        done = [r for r in runs if r.get("meta", {}).get("stage") == stage
                and r.get("cell") == cell and r.get("ok")]
        if done:
            print(f"[skip] {stage} {cell_label(cell)} (already ran)")
            continue
        meta = meta_of(args)
        meta["stage"] = stage
        meta["duration_s"] = duration
        meta["warmup_s"] = warmup
        print(f"[run ] {stage} {i + 1}/{len(cells)} {cell_label(cell)} "
              f"({duration}s, warmup {warmup}s)")
        try:
            result = run_cell(args.host, cell, duration, warmup, i, meta)
        except Exception as e:
            result = {"cell": cell, "ok": False,
                      "error": f"{type(e).__name__}: {e}",
                      "meta": meta, "index": i}
        if result.get("status_pre"):
            pass
        runs.append(result)
        with out_jsonl.open("a") as f:
            f.write(json.dumps(result) + "\n")
        if result.get("ok"):
            rx = result["rx"]
            print(f"[  ok] fps={rx['fps']['mean']} bytes_p50={rx['frame_bytes']['p50']} "
                  f"dev_capture_fps={result['device']['capture_fps']} "
                  f"dead={result.get('dead')} events={[e['type'] for e in result.get('events', [])]}",
                  flush=True)
            consecutive_errors = 0
        else:
            print(f"[fail] {result['error']}", flush=True)
            consecutive_errors += 1
            err_text = str(result.get("error", ""))
            needs_repair = ("no sensor" in err_text or
                            ("apply failed" in err_text and
                             cell.get("fbloc") != "dram"))
            if needs_repair:
                if recover_dead_camera(args.host) is None:
                    print("[abort] camera dead and no recovery",
                          flush=True)
                    break
                consecutive_errors = 0
            elif consecutive_errors >= 5:
                print("[abort] 5 consecutive failures — aborting stage",
                      flush=True)
                break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="all",
                    help="comma list: envelope,fbgrab,confirm,report")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--out", default=None, help="results base path")
    ap.add_argument("--list", action="store_true", help="print planned cells and exit")
    args = ap.parse_args()

    stages = (["envelope", "fbgrab", "confirm", "report"]
              if args.stages == "all" else args.stages.split(","))

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_jsonl = Path(args.out) if args.out else (
        Path(__file__).resolve().parent / "results" / f"phase4-{stamp}.jsonl")
    out_jsonl.parent.mkdir(parents=True, exist_ok=True)
    runs = load_jsonl(out_jsonl)

    if args.list:
        print("envelope:", len(envelope_cells()), "cells")
        print("fbgrab: 12 cells at boundary point (derived from envelope)")
        print("confirm: ladder candidates + first failing point, 130 s each")
        print(f"results: {out_jsonl}")
        return

    try:
        if "envelope" in stages:
            execute(args, envelope_cells(), "envelope", 55, 10, out_jsonl, runs)
        if "fbgrab" in stages:
            env = [r for r in runs if r.get("meta", {}).get("stage") == "envelope"]
            base = boundary_cell(env)
            if base is None:
                print("[skip] fbgrab: no >=15 fps envelope point found")
            else:
                print(f"[info] fbgrab base point: {cell_label(base)}")
                execute(args, fbgrab_cells(base), "fbgrab", 55, 10, out_jsonl, runs)
        if "confirm" in stages:
            env = [r for r in runs if r.get("meta", {}).get("stage") == "envelope"]
            fbg = [r for r in runs if r.get("meta", {}).get("stage") == "fbgrab"
                   and r.get("ok")]
            params = dict(BASELINE)
            if fbg:
                winner = pick_fbgrab_winner(fbg)
                if winner:
                    params = {k: winner[k] for k in ("fb_count", "grab", "fbloc")}
                    print(f"[info] fbgrab winner params: {params}")
            cells = confirm_cells(env, params)
            if not cells:
                print("[skip] confirm: no ladder candidates from envelope")
            else:
                for c in cells:
                    print(f"[plan] confirm {cell_label(c)}")
                execute(args, cells, "confirm", 130, 10, out_jsonl, runs)
        if "report" in stages:
            print_report(runs)
    finally:
        print(f"\nresults jsonl: {out_jsonl}")
        with out_jsonl.open("a") as f:
            f.write(json.dumps({"final_ladder": [
                {"cell": r["cell"], "fps": r["rx"]["fps"],
                 "frame_bytes": r["rx"]["frame_bytes"]}
                for r in derive_ladder(runs)]}) + "\n")


if __name__ == "__main__":
    main()
