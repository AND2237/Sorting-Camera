"""Smoke-check the fake camera against the ADR-0017 config contract (FW-13 / FW-10).

Starts ``tools/fake_camera.py`` on ephemeral-safe fixed ports, drives the control
plane the way a client must (GET is read-only, writes are POST + JSON), and reads
the MJPEG stream's raw wire bytes to prove the fake really emits the chunked
transfer the firmware sends (CP-8).

Usage (from the repository root):

    python tools/config_contract_smoke.py

Exit code = number of failed cases. Requires no hardware and no Qt.
"""
import http.client
import json
import socket
import subprocess
import sys
import time

PORT = 18080
STREAM = 18081
fails = []


def check(name, ok, detail=""):
    if ok:
        print("PASS  %s" % name)
    else:
        print("FAIL  %s  %s" % (name, detail))
        fails.append(name)


def req(method, path, body=None, ctype=None):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=6)
    headers = {}
    if ctype is not None:
        headers["Content-Type"] = ctype
    conn.request(method, path, body=body, headers=headers)
    r = conn.getresponse()
    data = r.read().decode(errors="replace")
    conn.close()
    return r.status, data


def probe_stream():
    """Read the raw stream head plus the first transfer chunk, unsatisfied."""
    sock = socket.create_connection(("127.0.0.1", STREAM), timeout=6)
    try:
        sock.sendall(b"GET /stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
        buf = b""
        deadline = time.time() + 6
        while b"\r\n\r\n" not in buf and time.time() < deadline:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, body = buf.partition(b"\r\n\r\n")
        while len(body) < 64 and time.time() < deadline:
            chunk = sock.recv(4096)
            if not chunk:
                break
            body += chunk
        return head.decode(errors="replace"), body
    finally:
        sock.close()


def stream_wire_cases():
    try:
        head, body = probe_stream()
    except OSError as exc:
        check("stream reachable", False, str(exc))
        return

    check("stream status 200", head.startswith("HTTP/1.1 200"), head[:60])
    lowered = head.lower()
    check("stream declares multipart and chunked",
          "multipart/x-mixed-replace" in lowered and "transfer-encoding: chunked" in lowered,
          head.replace("\r\n", " | ")[:160])

    first_line = body.split(b"\r\n", 1)[0]
    hex_size = False
    try:
        size = int(first_line, 16)
        hex_size = size > 0
    except ValueError:
        size = 0
    check("first transfer chunk is hex-framed and carries a --FRAME part",
          hex_size and b"--FRAME\r\n" in body,
          "chunk line %r, frame present=%s" % (first_line[:40], b"--FRAME\r\n" in body))


def main():
    proc = subprocess.Popen(
        [sys.executable, "tools/fake_camera.py",
         "--control-port", str(PORT), "--stream-port", str(STREAM),
         "--discovery-port", "48891", "--fault", "noauth",
         "--exit-after", "60"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(60):
            try:
                if req("GET", "/api/v1/status")[0] == 200:
                    break
            except Exception:
                time.sleep(0.2)
        else:
            check("startup", False, "fake camera never answered")
            return 1

        status, body = req("GET", "/api/v1/config")
        check("GET config 200", status == 200, "%s %s" % (status, body[:80]))
        state = json.loads(body)
        check("GET config returns state", "resolution" in state)

        status, body = req("GET", "/api/v1/config?framesize=hd")
        check("GET with query 400", status == 400, "%s %s" % (status, body[:80]))
        check("GET with query says read-only", "read-only" in body, body[:80])

        status, body = req("POST", "/api/v1/config?framesize=hd",
                           body=json.dumps({"framesize": "hd"}),
                           ctype="application/json")
        check("POST with query 400", status == 400, "%s %s" % (status, body[:80]))
        check("POST with query names it", "query string" in body, body[:80])

        status, body = req("POST", "/api/v1/config",
                           body=json.dumps({"quality": 12}),
                           ctype="application/json")
        check("POST json 200", status == 200, "%s %s" % (status, body[:80]))
        echo = json.loads(body)
        check("POST echo quality", echo.get("quality") == 12, str(echo.get("quality")))

        status, body = req("POST", "/api/v1/config",
                           body=json.dumps({"framesize": "hd"}))
        check("POST without content-type 415", status == 415, "%s %s" % (status, body[:80]))

        status, body = req("POST", "/api/v1/config",
                           body="framesize=hd", ctype="text/plain")
        check("POST text/plain 415", status == 415, "%s %s" % (status, body[:80]))

        status, body = req("POST", "/api/v1/config",
                           body=json.dumps({"nosuch": 1}),
                           ctype="application/json")
        check("POST unknown key 400", status == 400, "%s %s" % (status, body[:80]))
        check("POST unknown key names it", "nosuch" in body, body[:80])

        status, body = req("POST", "/api/v1/config",
                           body="[1,2]", ctype="application/json")
        check("POST non-object 400", status == 400, "%s %s" % (status, body[:80]))

        status, body = req("GET", "/api/v1/config")
        after = json.loads(body)
        check("write persisted", after.get("quality") == 12 and
              after.get("resolution") == state.get("resolution"),
              json.dumps(after)[:120])

        status, body = req("GET", "/api/v1/status")
        check("status still 200", status == 200, str(status))

        stream_wire_cases()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
    print("FAILURES=%d" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
