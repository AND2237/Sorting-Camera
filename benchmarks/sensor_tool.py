#!/usr/bin/env python3
"""Read and set the camera's sensor controls over the control API, and grab a snapshot.

Why this exists: with no saved sensor record (a new or erased chip) the firmware used
to start 11 controls at 0 instead of their defaults: auto exposure, auto gain,
exposure value, white balance, lens/pixel correction and dcw, the sensor's downscaler.
Auto exposure/gain off gives completely black frames (a black JPEG is about 10 KiB at
800x600 and 37 KiB at 1600x1200 whatever the quality, so every frame-size and fps
measurement is meaningless), and dcw off leaves XGA/SXGA/HD frames at 1600x1200.
Check the sensor first, then measure.

    python benchmarks/sensor_tool.py --info
    python benchmarks/sensor_tool.py --restore-defaults      # every control back to its documented default
    python benchmarks/sensor_tool.py --fix-exposure          # only aec, aec2, agc, awb, awb_gain on
    python benchmarks/sensor_tool.py --set "gainceiling=4,brightness=1"
    python benchmarks/sensor_tool.py --snapshot snap.jpg     # save one frame, report its brightness

Controls set here are saved by the device to its NVS and restored after every camera
re-initialisation (frame-size change, recovery), so they hold for later test runs.
The device refuses sensor changes while a stream client is connected.

Brightness is measured automatically only if Pillow is installed (pip install
pillow); otherwise open the saved .jpg and look at it. The control password comes from
--password, SCAM_CONTROL_PASSWORD or the git-ignored config_secrets.h (never printed).
"""

import argparse
import io
import json
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_bench as nb  # noqa: E402
import phase5_pipeline as p5  # noqa: E402

# The firmware's documented defaults for the controls that decide whether the
# picture is visible at all (camera_control.c s_defs).
AUTO_ON = {"aec": 1, "aec2": 1, "agc": 1, "awb": 1, "awb_gain": 1}
# Controls whose value 0 is wrong by default, found by comparing the firmware's table
# with what its init does when nothing is saved (see docs/decisions/0019).
SUSPECT_ZERO = ["aec", "aec2", "aec_value", "agc", "awb", "awb_gain", "bpc", "wpc", "raw_gma",
                "lenc", "dcw"]
KEY_ORDER = ["aec", "aec2", "aec_value", "agc", "agc_gain", "gainceiling", "awb", "awb_gain",
             "bpc", "wpc", "raw_gma", "lenc", "dcw",
             "brightness", "contrast", "saturation", "special_effect"]


class Api:
    def __init__(self, host, port, password):
        self.host, self.port, self.password = host, port, password
        self.token = None

    def _tok(self, force=False):
        if self.token and not force:
            return self.token
        self.token = p5.authenticate(self.host, self.password, self.port) if self.password else None
        return self.token

    def json(self, method, path, body=None, timeout=15.0):
        status, obj, text = 0, None, ""
        for attempt in (1, 2):
            status, obj, text = p5.raw_request(self.host, path, self.port, timeout, method, body,
                                               self._tok(force=(attempt == 2)))
            if status != 401:
                break
        return status, obj, text

    def bytes_get(self, path, timeout=15.0):
        """GET a binary body (the snapshot). Returns (status, bytes)."""
        for attempt in (1, 2):
            tok = self._tok(force=(attempt == 2))
            head = f"GET {path} HTTP/1.1\r\nHost: {self.host}\r\nConnection: close\r\n"
            if tok:
                head += f"Authorization: Bearer {tok}\r\n"
            head += "\r\n"
            try:
                s = socket.create_connection((self.host, self.port), timeout=timeout)
            except OSError:
                return 0, b""
            s.settimeout(timeout)
            s.sendall(head.encode())
            buf = b""
            try:
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
            except OSError:
                pass
            s.close()
            raw_head, _, body = buf.partition(b"\r\n\r\n")
            lines = raw_head.decode(errors="replace").split("\r\n")
            status = int(lines[0].split()[1]) if lines and lines[0].startswith("HTTP/") else 0
            if status != 401:
                clen = None
                for ln in lines[1:]:
                    if ln.lower().startswith("content-length:"):
                        clen = int(ln.split(":", 1)[1])
                return status, (body[:clen] if clen is not None else body)
        return 401, b""


def show_sensor(sens):
    keys = [k for k in KEY_ORDER if k in sens] + sorted(k for k in sens if k not in KEY_ORDER)
    print("sensor controls (current values on the device):")
    for k in keys:
        print(f"  {k:<16} {sens[k]}")
    problems = []
    if sens.get("aec") == 0:
        problems.append(f"aec (auto exposure) is OFF; manual exposure aec_value={sens.get('aec_value')}"
                        + (" -> an exposure of 0 gives a black picture" if sens.get("aec_value") == 0 else ""))
    if sens.get("agc") == 0:
        problems.append(f"agc (auto gain) is OFF; manual gain agc_gain={sens.get('agc_gain')}")
    if problems:
        print("\nLIKELY CAUSE OF BLACK FRAMES:")
        for p in problems:
            print("  - " + p)
    else:
        print("\nauto exposure and auto gain are on.")
    zeros = [k for k in SUSPECT_ZERO if sens.get(k) == 0]
    if zeros:
        print(f"\ncontrols that read 0 and normally default to on/non-zero: {', '.join(zeros)}")
        if "dcw" in zeros:
            print("  dcw = 0 disables the sensor's downscaler: XGA/SXGA/HD frames then come out at 1600x1200.")
        print("  fix: python benchmarks/sensor_tool.py --restore-defaults")


def parse_set(spec):
    out = {}
    for part in spec.split(","):
        if not part.strip():
            continue
        k, _, v = part.partition("=")
        if not k.strip() or not v.strip().lstrip("-").isdigit():
            raise SystemExit(f"bad --set item '{part}': use name=integer")
        out[k.strip()] = int(v)
    return out


def jpeg_report(data):
    dims = nb.jpeg_dims(data[:1024])
    ok_soi = data[:2] == b"\xff\xd8"
    ok_eoi = data[-2:] == b"\xff\xd9"
    print(f"snapshot: {len(data)} bytes ({len(data) / 1024:.1f} KiB), size {dims}, "
          f"start marker {'ok' if ok_soi else 'MISSING'}, end marker {'ok' if ok_eoi else 'MISSING'}")
    try:
        from PIL import Image, ImageStat  # noqa: PLC0415
    except ImportError:
        print("brightness: not measured (Pillow not installed: pip install pillow). "
              "Open the saved .jpg and check that the scene is visible.")
        return None
    img = Image.open(io.BytesIO(data)).convert("L")
    mean = ImageStat.Stat(img).mean[0]
    hist = img.histogram()
    dark_pct = 100.0 * sum(hist[:16]) / sum(hist)
    verdict = "BLACK/near-black: the scene is not visible" if mean < 10 else (
        "dark" if mean < 40 else "ok")
    print(f"brightness: mean {mean:.1f} / 255, {dark_pct:.1f}% of pixels below 16 -> {verdict}")
    return mean


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=nb.HOST_DEFAULT)
    ap.add_argument("--control-port", type=int, default=80)
    ap.add_argument("--password", default=None)
    ap.add_argument("--no-auth", action="store_true")
    ap.add_argument("--info", action="store_true", help="print the sensor controls")
    ap.add_argument("--fix-exposure", action="store_true",
                    help="switch automatic exposure, exposure2, gain and white balance on")
    ap.add_argument("--restore-defaults", action="store_true",
                    help="set every supported control to the default the device itself documents")
    ap.add_argument("--set", default=None, help='controls to set, e.g. "gainceiling=4,brightness=1"')
    ap.add_argument("--snapshot", default=None, metavar="FILE", help="save one frame and check it")
    args = ap.parse_args()
    if not (args.info or args.fix_exposure or args.restore_defaults or args.set or args.snapshot):
        args.info = True

    password = None if args.no_auth else p5.read_password(args.password)
    api = Api(args.host, args.control_port, password)

    status, obj, _ = p5.raw_request(args.host, "/api/v1/capabilities", args.control_port, 6.0)
    if status == 0:
        raise SystemExit(f"cannot reach {args.host}:{args.control_port}. Joined to the camera's Wi-Fi?")
    if status == 401 and not password:
        raise SystemExit("the device requires a control password: pass --password or set "
                         "SCAM_CONTROL_PASSWORD")

    s, sens, text = api.json("GET", "/api/v1/sensor")
    if s != 200 or not sens:
        raise SystemExit(f"cannot read sensor controls (HTTP {s}) {text[:100]}")

    if args.info:
        show_sensor(sens)

    changes = {}
    if args.restore_defaults:
        c, caps, ctext = api.json("GET", "/api/v1/capabilities")
        ctl = (caps or {}).get("controls") if c == 200 else None
        if not ctl:
            raise SystemExit(f"cannot read the control defaults from /api/v1/capabilities (HTTP {c})")
        for name, entry in ctl.items():
            if entry.get("supported") and "default" in entry and name in sens:
                changes[name] = int(entry["default"])
        diff = {k: (sens[k], v) for k, v in changes.items() if sens.get(k) != v}
        print("controls that will change (current -> default): " + (", ".join(
            f"{k} {a}->{b}" for k, (a, b) in diff.items()) or "none"))
    if args.fix_exposure:
        changes.update(AUTO_ON)
    if args.set:
        changes.update(parse_set(args.set))
    if changes:
        print(f"\nsetting {len(changes)} control(s)")
        items = list(changes.items())
        for i in range(0, len(items), 8):  # the endpoint accepts at most 512 bytes of JSON
            s, obj, text = api.json("POST", "/api/v1/sensor", dict(items[i:i + 8]))
            if s != 200:
                raise SystemExit(f"device refused (HTTP {s}): {text[:200]}")
        s, sens, _ = api.json("GET", "/api/v1/sensor")
        print("applied and saved on the device. Now:")
        show_sensor(sens or {})

    if args.snapshot:
        import time
        if changes:
            time.sleep(1.5)  # let the auto-exposure loop settle on a few frames
        for _ in range(3):  # the first frame after a change can be stale
            st, data = api.bytes_get("/api/v1/snapshot")
        if st != 200 or not data:
            raise SystemExit(f"snapshot failed (HTTP {st})")
        Path(args.snapshot).write_bytes(data)
        print(f"saved {args.snapshot}")
        jpeg_report(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
