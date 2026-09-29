"""Fake ESP32-CAM for hardware-free integration testing (Phase 6, item C3).

Serves the same HTTP/UDP surface as the real firmware so the desktop app,
the unit tests and the benchmark harness can be exercised without hardware
and without reflashing:

  control  GET  /api/v1/status | /api/v1/config | /api/v1/capabilities
           GET  /api/v1/sensor
           POST /api/v1/sensor
           GET  /api/v1/auth/challenge
           POST /api/v1/auth/login
           POST /api/v1/auth/logout
           GET  /api/v1/snapshot
  stream   GET  /stream            multipart/x-mixed-replace, chunked
  discovery UDP  <-- {"scam":1,"op":"discover"}   unicast announce

The authentication scheme matches the firmware: PBKDF2-HMAC-SHA256 verifier
with a 16-byte salt, then HMAC-SHA256(verifier, nonce) as the proof.

Frames are real OV2640 JPEGs (captured at QQVGA) in tools/fixtures/ so the
decode and render path is exercised with valid data. Set --fps to change the
cadence; the app's latest-frame-wins path is what is under test.

Fault injection (comma separated, for error-path tests):
  noauth          device was never provisioned: /api/v1/* is unauthenticated
  always401       every protected endpoint answers 401
  lockout         reject the first N proofs, then lock out with 429
  slowconfig      delay config responses
  config409       answer 409 to config while a stream client is attached
  error500        answer 500 to status
  nostream        refuse the stream port
  nodiscovery     do not answer discovery queries
"""

import argparse
import hashlib
import hmac
import json
import os
import secrets
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SALT = bytes.fromhex("00112233445566778899aabbccddeeff")
ITERATIONS = 8192
NONCE_TTL = 30
TOKEN_TTL = 1800
MAX_SESSIONS = 4

RESOLUTIONS = [
    {"key": "qqvga", "width": 160, "height": 120, "quality_floor": 0, "xclk_max_mhz": 27},
    {"key": "qvga", "width": 320, "height": 240, "quality_floor": 0, "xclk_max_mhz": 27},
    {"key": "vga", "width": 640, "height": 480, "quality_floor": 2, "xclk_max_mhz": 27},
    {"key": "svga", "width": 800, "height": 600, "quality_floor": 2, "xclk_max_mhz": 27},
    {"key": "xga", "width": 1024, "height": 768, "quality_floor": 6, "xclk_max_mhz": 27},
    {"key": "hd", "width": 1280, "height": 720, "quality_floor": 6, "xclk_max_mhz": 20},
    {"key": "sxga", "width": 1280, "height": 1024, "quality_floor": 8, "xclk_max_mhz": 27},
    {"key": "uxga", "width": 1600, "height": 1200, "quality_floor": 16, "xclk_max_mhz": 27},
]

CONTROLS = {
    "brightness": (-2, 2, 0, True, "image", None),
    "contrast": (-2, 2, 0, True, "image", None),
    "saturation": (-2, 2, 0, True, "image", None),
    "ae_level": (-2, 2, 0, True, "image", None),
    "special_effect": (0, 6, 0, True, "image", None),
    "wb_mode": (0, 4, 0, True, "white_balance", None),
    "aec_value": (0, 1200, 300, True, "exposure",
                  "manual exposure time, ignored while aec is on"),
    "agc_gain": (0, 30, 0, True, "gain",
                 "manual analog gain, ignored while agc is on"),
    "gainceiling": (0, 6, 0, True, "gain", "2x 4x 8x 16x 32x 64x 128x"),
    "agc": (0, 1, 1, True, "gain", "automatic gain control"),
    "aec": (0, 1, 1, True, "exposure", "automatic exposure control"),
    "aec2": (0, 1, 1, True, "exposure", None),
    "awb": (0, 1, 1, True, "white_balance", "automatic white balance"),
    "awb_gain": (0, 1, 1, True, "white_balance", "automatic white balance gain"),
    "hmirror": (0, 1, 0, True, "orientation", None),
    "vflip": (0, 1, 0, True, "orientation", None),
    "bpc": (0, 1, 1, True, "processing", "black pixel correction"),
    "wpc": (0, 1, 1, True, "processing", "white pixel correction"),
    "raw_gma": (0, 1, 1, True, "processing", "raw gamma"),
    "lenc": (0, 1, 1, True, "processing", "lens correction"),
    "dcw": (0, 1, 1, True, "processing", "DCW"),
    "colorbar": (0, 1, 0, True, "diagnostic", "test pattern, not a product feature"),
    "sharpness": (-2, 2, 0, False, "image",
                  "OV2640 driver stub returns -1, not implemented in hardware"),
    "denoise": (0, 0, 0, False, "processing",
                "OV2640 driver stub returns -1, not implemented in hardware"),
}


class State:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.frames = load_frames(args.frames)
        self.config = {
            "resolution": "hd",
            "quality": 12,
            "xclk_mhz": 18,
            "fb_count": 3,
            "grab_mode": "latest",
            "fb_location": "psram",
        }
        self.sensor = {name: spec[2] for name, spec in CONTROLS.items() if spec[3]}
        self.tokens = {}
        self.nonce = None
        self.failures = 0
        self.attempts = 0
        self.started = time.time()
        self.stream_clients = 0
        self.snapshots = 0
        self.verifier = None
        if args.password:
            self.verifier = hashlib.pbkdf2_hmac(
                "sha256", args.password.encode(), SALT, ITERATIONS, 32)

    @property
    def auth_required(self):
        return "noauth" not in self.args.fault and self.verifier is not None

    def protected(self):
        return "noauth" not in self.args.fault

    def issue_nonce(self):
        self.nonce = (secrets.token_bytes(16), time.time() + NONCE_TTL)
        return self.nonce

    def check_proof(self, nonce_hex, proof_hex):
        if self.nonce is None or time.time() > self.nonce[1]:
            return False, "challenge expired or already used, request a new one"
        nonce = self.nonce[0]
        self.nonce = None
        if nonce_hex is None or proof_hex is None:
            return False, "nonce and proof are required"
        try:
            nonce_bytes = bytes.fromhex(nonce_hex)
            proof = bytes.fromhex(proof_hex)
        except ValueError:
            return False, "malformed nonce or proof"
        if nonce_bytes != nonce:
            return False, "challenge expired or already used, request a new one"
        expected = hmac.new(self.verifier, nonce, hashlib.sha256).digest()
        if not hmac.compare_digest(proof, expected):
            self.failures += 1
            if self.failures >= 8:
                self.failures = 0
                return False, "too many failures, client locked for 300s"
            return False, "invalid proof"
        self.failures = 0
        token = secrets.token_hex(16)
        self.tokens[token] = time.time() + TOKEN_TTL
        return True, token


def load_frames(path):
    frames = []
    if os.path.isdir(path):
        for name in sorted(os.listdir(path)):
            if name.lower().endswith(".jpg"):
                with open(os.path.join(path, name), "rb") as handle:
                    frames.append(handle.read())
    elif os.path.isfile(path):
        with open(path, "rb") as handle:
            frames.append(handle.read())
    if not frames:
        raise SystemExit("no JPEG frames found at %s" % path)
    return frames


def make_handler(state, kind):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            if state.args.verbose:
                sys.stderr.write("[%s] %s\n" % (kind, fmt % args))

        # -- helpers -----------------------------------------------------
        def send_json(self, code, payload):
            body = json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def send_text(self, code, text):
            body = text.encode()
            self.send_response(code)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def read_json(self):
            length = int(self.headers.get("Content-Length", 0))
            if length <= 0 or length > 4096:
                return None
            try:
                return json.loads(self.rfile.read(length).decode())
            except ValueError:
                return None

        def token(self):
            header = self.headers.get("Authorization", "")
            if not header.startswith("Bearer "):
                return None
            value = header[7:].strip()
            with state.lock:
                expiry = state.tokens.get(value)
                if expiry and time.time() < expiry:
                    return value
            return None

        def guard(self):
            if not state.protected():
                return True
            if "always401" in state.args.fault:
                self.send_json(401, {"error": "unauthenticated",
                                     "hint": "POST /api/v1/auth/login"})
                return False
            if not self.token():
                self.send_json(401, {"error": "unauthenticated",
                                     "hint": "POST /api/v1/auth/login"})
                return False
            return True

        # -- routing -----------------------------------------------------
        def do_GET(self):
            if kind == "stream":
                return self.serve_stream()
            path = self.path.split("?")[0]
            if path == "/api/v1/auth/challenge":
                return self.auth_challenge()
            if path.startswith("/api/v1/"):
                if not self.guard():
                    return None
            if path == "/api/v1/status":
                return self.status()
            if path == "/api/v1/config":
                return self.config_get()
            if path == "/api/v1/capabilities":
                return self.capabilities()
            if path == "/api/v1/sensor":
                return self.send_json(200, dict(state.sensor))
            if path == "/api/v1/snapshot":
                frame = state.frames[0]
                with state.lock:
                    state.snapshots += 1
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(frame)))
                self.end_headers()
                self.wfile.write(frame)
                return None
            return self.send_text(404, "not found")

        def do_POST(self):
            path = self.path.split("?")[0]
            if path == "/api/v1/auth/login":
                return self.auth_login()
            if path == "/api/v1/auth/logout":
                if not self.guard():
                    return None
                value = self.token()
                with state.lock:
                    state.tokens.pop(value, None)
                return self.send_json(200, {"logged_out": True})
            if not self.guard():
                return None
            if path == "/api/v1/sensor":
                return self.sensor_post()
            if path == "/api/v1/config":
                return self.config_apply()
            return self.send_text(404, "not found")

        # -- endpoints ---------------------------------------------------
        def auth_challenge(self):
            if not state.auth_required:
                return self.send_text(409, "authentication not provisioned on this device")
            with state.lock:
                nonce, _ = state.issue_nonce()
            return self.send_json(200, {
                "nonce": nonce.hex(),
                "salt": SALT.hex(),
                "algo": "PBKDF2-HMAC-SHA256/HMAC-SHA256",
                "iterations": ITERATIONS,
                "nonce_ttl_s": NONCE_TTL,
            })

        def auth_login(self):
            payload = self.read_json() or {}
            with state.lock:
                state.attempts += 1
                if "lockout" in state.args.fault and state.attempts <= 3:
                    return self.send_text(401, "invalid proof")
                ok, result = state.check_proof(payload.get("nonce"),
                                               payload.get("proof"))
            if not ok:
                return self.send_text(401, result)
            return self.send_json(200, {"token": result, "expires_in_s": TOKEN_TTL,
                                        "device_id": state.args.device_id})

        def status(self):
            if "error500" in state.args.fault:
                return self.send_text(500, "internal error")
            with state.lock:
                payload = {
                    "fw_version": state.args.fw_version,
                    "device_name": "fake-cam-01",
                    "proto_version": 1,
                    "uptime_s": int(time.time() - state.started),
                    "free_heap": 3500000,
                    "free_spiram": 3400000,
                    "rssi": -42,
                    "frames_captured": int((time.time() - state.started) * 10),
                    "frames_delivered": int((time.time() - state.started) * 10),
                    "capture_failures": 0,
                    # The fake never drops a frame, so these stay 0 - they are
                    # present so the shape matches the firmware's status JSON.
                    "frames_send_failures": 0,
                    "frames_near_budget": 0,
                    "snapshots_served": state.snapshots,
                    "avg_capture_ms": 0.4,
                    "last_frame_bytes": len(state.frames[0]),
                    "stream_clients": state.stream_clients,
                    "camera_recoveries": 0,
                    "camera_up": 1,
                    "frame_budget_bytes": 262144,
                    "quality_floor": 6,
                    "xclk_max_mhz": 20,
                    "reset_reason": 1,
                    "resolution": dict((r["key"], "%dx%d" % (r["width"], r["height"]))
                                       for r in RESOLUTIONS)[state.config["resolution"]],
                    "quality": state.config["quality"],
                    "fb_count": state.config["fb_count"],
                    "xclk_mhz": state.config["xclk_mhz"],
                    "grab_mode": state.config["grab_mode"],
                    "fb_location": state.config["fb_location"],
                    "device_id": state.args.device_id,
                    "tcp_clients": 0,
                    "udp_peer": 0,
                    "udp_tx_dgrams": 0,
                    "udp_tx_drops": 0,
                }
            return self.send_json(200, payload)

        def capabilities(self):
            controls = {}
            for name, (low, high, default, supported, group, note) in CONTROLS.items():
                entry = {"supported": supported, "group": group, "min": low,
                         "max": high, "default": default}
                if note:
                    entry["note"] = note
                controls[name] = entry
            return self.send_json(200, {
                "fw_version": state.args.fw_version,
                "proto_version": 1,
                "device_name": "fake-cam-01",
                "device_id": state.args.device_id,
                "ip": "127.0.0.1",
                "sensor": "ov2640",
                "control_port": state.args.control_port,
                "stream_port": state.args.stream_port,
                "discovery_port": state.args.discovery_port,
                "frame_budget_bytes": 262144,
                "resolutions": RESOLUTIONS,
                "controls": controls,
                "features": {"autofocus": False, "raw_register_access": False,
                             "sensor_controls_live": True, "quality_live": True},
                "auth": {"required": state.auth_required, "challenge_response": True,
                         "algo": "PBKDF2-HMAC-SHA256/HMAC-SHA256" if state.auth_required else "none",
                         "iterations": ITERATIONS if state.auth_required else 0,
                         "token_ttl_s": TOKEN_TTL, "active_sessions": len(state.tokens),
                         "stream_port_protected": False},
            })

        def sensor_post(self):
            payload = self.read_json()
            if not isinstance(payload, dict):
                return self.send_text(400, "invalid JSON body")
            planned = []
            for name, value in payload.items():
                if name not in CONTROLS:
                    return self.send_text(400, "unknown control '%s'" % name)
                low, high, _default, supported, _group, note = CONTROLS[name]
                if not supported:
                    return self.send_text(400, "control '%s' is not supported: %s"
                                          % (name, note))
                if isinstance(value, bool):
                    value = 1 if value else 0
                if not isinstance(value, int) or value < low or value > high:
                    return self.send_text(400, "control '%s' out of range: %s not in [%d, %d]"
                                          % (name, value, low, high))
                planned.append((name, value))
            with state.lock:
                for name, value in planned:
                    state.sensor[name] = value
                snapshot = dict(state.sensor)
            return self.send_json(200, snapshot)

        CONFIG_KEYS = ("framesize", "quality", "xclk", "fb_count", "grab", "fbloc")

        def config_get(self):
            # Read-only since FW-13, and a query string on a GET is either a
            # write attempt or a truncated one. Both used to be answered with a
            # 200 describing a camera nobody had touched (FW-10), so the fake
            # refuses exactly the way the firmware now does - a benchmark that
            # only talks to the fake must not discover the change first.
            if "?" in self.path:
                return self.send_text(
                    400, "GET /api/v1/config is read-only; send writes as "
                         "POST /api/v1/config with a JSON body")
            return self.send_json(200, dict(state.config))

        def config_apply(self):
            # The content type is the CSRF contract, not decoration (ADR-0017),
            # and it is checked before anything is read - which is the order the
            # firmware uses, so a test cannot pass here and fail there.
            if "application/json" not in self.headers.get("Content-Type", ""):
                return self.send_text(
                    415, "config writes require Content-Type: application/json")
            payload = self.read_json()
            if not isinstance(payload, dict):
                return self.send_text(400, "body must be a JSON object")
            # Read first, then refuse, so the request is fully consumed before
            # the connection is answered (the firmware does the same).
            if "?" in self.path:
                return self.send_text(
                    400, "config writes carry no query string; put every "
                         "setting in the JSON body")
            unknown = [k for k in payload if k not in self.CONFIG_KEYS]
            if unknown:
                return self.send_text(400, "unknown config key: %s" % unknown[0])
            if "config409" in state.args.fault and state.stream_clients > 0:
                return self.send_text(409, "stream active: disconnect before config change")
            if "slowconfig" in state.args.fault:
                time.sleep(state.args.slow_seconds)
            with state.lock:
                if "framesize" in payload:
                    keys = [r["key"] for r in RESOLUTIONS]
                    if payload["framesize"] not in keys:
                        return self.send_text(400, "bad framesize")
                    state.config["resolution"] = payload["framesize"]
                if "quality" in payload:
                    value = int(payload["quality"])
                    floor = next(r["quality_floor"] for r in RESOLUTIONS
                                 if r["key"] == state.config["resolution"])
                    if value < floor:
                        return self.send_text(400, "quality %d is below the measured safe "
                                                    "floor %d" % (value, floor))
                    state.config["quality"] = value
                if "xclk" in payload:
                    value = int(payload["xclk"])
                    ceiling = next(r["xclk_max_mhz"] for r in RESOLUTIONS
                                   if r["key"] == state.config["resolution"])
                    if value < 6 or value > 27 or value > ceiling:
                        return self.send_text(400, "xclk %d MHz is above the measured "
                                                    "ceiling %d MHz" % (value, ceiling))
                    state.config["xclk_mhz"] = value
                for key, low, high in (("fb_count", 1, 3),):
                    if key in payload:
                        state.config[key] = int(payload[key])
                if "grab" in payload:
                    state.config["grab_mode"] = payload["grab"]
                return self.send_json(200, dict(state.config))

        def serve_stream(self):
            if "nostream" in state.args.fault:
                return self.send_text(503, "stream unavailable")
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace;boundary=FRAME")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            with state.lock:
                state.stream_clients += 1
            index = 0
            interval = 1.0 / max(state.args.fps, 0.1)
            try:
                while True:
                    frame = state.frames[index % len(state.frames)]
                    index += 1
                    header = (b"--FRAME\r\nContent-Type: image/jpeg\r\nContent-Length: "
                              + str(len(frame)).encode() + b"\r\n\r\n")
                    payload = header + frame + b"\r\n"
                    self.wfile.write(b"%X\r\n" % len(payload) + payload + b"\r\n")
                    self.wfile.flush()
                    time.sleep(interval)
            except (BrokenPipeError, ConnectionResetError, OSError):
                return None
            finally:
                with state.lock:
                    state.stream_clients -= 1

    return Handler


def discovery_thread(state, stop):
    if "nodiscovery" in state.args.fault:
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", state.args.discovery_port))
    controls = {}
    for name, spec in CONTROLS.items():
        if spec[3]:
            controls.setdefault(spec[4], []).append(name)
    last = 0
    while not stop.is_set():
        sock.settimeout(0.5)
        try:
            data, peer = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            query = json.loads(data.decode())
        except ValueError:
            continue
        if query.get("scam") != 1 or query.get("op") != "discover":
            continue
        now = time.time()
        if now - last < 0.25:
            continue
        last = now
        reply = {
            "scam": 1,
            "op": "announce",
            "device_id": state.args.device_id,
            "device_name": "fake-cam-01",
            "ip": "127.0.0.1",
            "fw_version": state.args.fw_version,
            "proto_version": 1,
            "sensor": "ov2640",
            "control_port": state.args.control_port,
            "stream_port": state.args.stream_port,
            "auth_required": state.auth_required,
            "resolutions": [r["key"] for r in RESOLUTIONS],
            "controls": controls,
        }
        sock.sendto(json.dumps(reply).encode(), peer)
        sys.stderr.write("[discovery] answered %s\n" % (peer[0],))


def main():
    parser = argparse.ArgumentParser(description="Fake ESP32-CAM for integration tests")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--control-port", type=int, default=8080)
    parser.add_argument("--stream-port", type=int, default=8081)
    parser.add_argument("--discovery-port", type=int, default=48889)
    parser.add_argument("--password", default="test-password",
                        help="control password; empty means no authentication")
    parser.add_argument("--device-id", default="facedeff0011")
    parser.add_argument("--fw-version", default="0.1.0-fake")
    parser.add_argument("--frames", default=os.path.join(here, "fixtures"))
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--slow-seconds", type=float, default=5.0)
    parser.add_argument("--fault", default="",
                        help="comma separated: noauth,always401,lockout,slowconfig,"
                             "config409,error500,nostream,nodiscovery")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--exit-after", type=float, default=0.0,
                        help="stop automatically after N seconds (0 = run until killed)")
    args = parser.parse_args()
    args.fault = set(f for f in args.fault.split(",") if f)

    state = State(args)
    control = ThreadingHTTPServer(("127.0.0.1", args.control_port),
                                  make_handler(state, "control"))
    stream = ThreadingHTTPServer(("127.0.0.1", args.stream_port),
                                 make_handler(state, "stream"))
    control.daemon_threads = True
    stream.daemon_threads = True

    stop = threading.Event()
    threading.Thread(target=discovery_thread, args=(state, stop), daemon=True).start()
    threading.Thread(target=control.serve_forever, daemon=True).start()
    threading.Thread(target=stream.serve_forever, daemon=True).start()

    sys.stderr.write(
        "[fake] control=127.0.0.1:%d stream=127.0.0.1:%d discovery=udp/%d auth=%s "
        "device_id=%s frames=%d faults=%s\n"
        % (args.control_port, args.stream_port, args.discovery_port,
           "on" if state.auth_required else "off", args.device_id,
           len(state.frames), ",".join(sorted(args.fault)) or "none"))
    sys.stderr.flush()

    try:
        if args.exit_after > 0:
            time.sleep(args.exit_after)
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        control.shutdown()
        stream.shutdown()


if __name__ == "__main__":
    main()
