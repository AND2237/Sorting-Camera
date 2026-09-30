# Protocol

_Status: selected 2026-09-25 — TCP framed is the primary transport (ADR-0007); HTTP MJPEG retained as compatibility path; UDP packetized implemented but secondary. Framing below is normative for TCP/UDP._

## Control plane (decided)

HTTP/1.1 + JSON over TCP port 80 on the camera (separate from video port where the transport requires it). All control endpoints require an authenticated session token (see `docs/security.md`).

Endpoints (initial):

| Method/path | Purpose |
|---|---|
| `GET /api/v1/status` | device info, camera state, metrics snapshot (rate-limited fields ok). Camera-state fields include `resolution`, `quality`, `fb_count`, `grab_mode`, `fb_location`, `reset_reason` (`esp_reset_reason()`), `stream_clients`, `camera_up`, `camera_recoveries`, `frame_budget_bytes`, `quality_floor`, stream/TCP/UDP client counters, and the loss/pressure counters `frames_send_failures`, `frames_near_budget`, `snapshots_served` (defined under Freshness policy). |
| `POST /api/v1/auth/login` | challenge/response → session token |
| `POST /api/v1/auth/logout` | invalidate token |
| `GET /api/v1/config`, `POST /api/v1/config` | read / write resolution, JPEG quality, transport/camera tuning. **GET is read-only** and returns the current state; a GET that carries a query string is answered **400**, because it is either an attempted write or one that arrived truncated (ADR-0017 — the old fall-through answered 200 about a camera nobody had touched, FW-10). **Writes are `POST` with a 1–512-byte JSON object** holding any of `framesize`, `quality`, `xclk`, `fb_count`, `grab`, `fbloc` (`fb_count` 1–3, `grab` `latest|cont`, `fbloc` `psram|dram`, `xclk` 6–27 MHz); it echoes the resulting state. A POST carrying a query string is **400** (`config writes carry no query string`), a missing or non-`application/json` `Content-Type` is **415**, an unknown key or a wrong-typed value is **400** — nothing is applied, because the body is read and validated in full before anything is taken from it. Apply is atomic: validate the whole body first, then a single deinit/init with rollback to the previous config on failure. Returns **409 Conflict** if the body contains any write while a stream or transport client is connected (config changes require stream disconnect — live change wedged the control server, see benchmark-results). Returns **400** with `camera apply failed: <esp_err_name>` on driver failure — including `ESP_ERR_NO_MEM`, which is the firmware's pre-check rejecting an infeasible DRAM frame-buffer request (largest internal block < `w*h/5` per buffer) — and **400** with the measured-floor message for a quality below `quality_floor`. The response also carries `measured_max_frame_bytes`: the largest frame **measured at that resolution's quality floor**, an upper bound for every accepted quality and an *under*-estimate of what a below-floor quality would produce (there is deliberately no measured size-vs-quality model, ADR-0009 decision 2 — so the field is named for what was measured rather than estimated for the requested quality). **Self-repair:** applying a config re-runs sensor power-cycle + SCCB bus recovery if the camera is down (a dead sensor no longer returns 500 `no sensor`; the config endpoint is the recovery path). |
| `GET /api/v1/sensor`, `POST /api/v1/sensor` | read / write sensor registers (brightness, contrast, saturation, …). `POST` takes a 1–512-byte JSON body and returns **409 Conflict** under the *same* rule as `/api/v1/config` — any write while a stream or transport client is connected is refused — and serialises against the camera lock so an SCCB write cannot run across a `camera_recover()` teardown/reinit. |
| `GET /api/v1/capabilities` | supported resolutions, control ranges, fw/protocol versions |

## Video plane candidates

### A. HTTP multipart MJPEG (baseline)

Standard `multipart/x-mixed-replace` stream. Zero framing invention; easiest correctness; known issues: head-of-line blocking, boundary parsing cost, single client per handler task, no per-frame metadata beyond `Content-Length`.

### B. TCP framed JPEG (candidate)

Single persistent TCP connection. Every frame:

```
offset  size  field
0       4     magic "SCAM"
4       1     protocol version (u8)
5       1     header length (u8) — allows future extension
6       2     device ID (u16, assigned/stable within fleet; low byte = uid-derived)
8       4     frame sequence (u32, wraps)
12      8     device timestamp (u64, µs since boot)
20      2     width (u16)
22      2     height (u16)
24      1     pixel format (u8; 0 = JPEG)
25      1     compression (u8; 0 = JPEG baseline)
26      4     payload length (u32)
30      2     flags (u16; bit0 = keyframe/resume marker, bit1 = config-changed, rest reserved 0)
32      4     CRC32 of payload (u32)
36      N     JPEG payload
```

Field justifications:

- `magic + version + header_len` — unambiguous resync, explicit compatibility (req. §7, §39).
- `device_id` — multi-camera demux without separate connections (req. §27).
- `seq` — loss/jitter measurement + stale detection (req. §8, §13).
- `timestamp` — device-side latency decomposition (req. §14).
- `width/height/format/compression` — self-describing frame, no out-of-band config to desync (req. §7).
- `payload length` — TCP stream reassembly (req. §7).
- `flags` — sparse needs only (resume after reconnect, notice of mid-stream config change); reserved bits keep it extensible without breaking v1 parsers.
- `CRC32` — detect corruption on memory-constrained path cheaply; TCP already checksums, so this also covers device-memory DMA glitches (measured use will decide keep/flag-disable).

Total overhead 36 B/frame — negligible vs JPEG payload sizes; no field added "because it sounds useful".

### C. UDP packetized JPEG (candidate)

Frame split into fixed-MTU (1400 B) datagrams; per-datagram header: frame seq, fragment index/count, frame CRC. Incomplete frame ⇒ drop whole frame (counted). Optional later: FEC — only if benchmarks show loss is a real problem on this LAN (req. §43).

Tradeoffs to measure: CPU on ESP32, loss behavior on the direct AP link, multi-client scaling, stale-frame behavior, reconnection simplicity.

## Versioning & compatibility

- `protocol version` in every custom frame/header; control API uses `/api/v1/` prefix.
- Desktop rejects major-version mismatch with a clear UI error; minor additions are backward-compatible (header_len / flags reserved bits).
- Firmware version, protocol version, app version each reported in status/discovery and recorded in benchmark results (req. §39, §13).

## Freshness policy (all transports)

Latest-complete-frame-wins end to end: if decode/render falls behind, older frames are dropped and counted (`frames_dropped`), never queued without bound (req. §8).

### Loss counters (normative definitions)

Everything that loses a frame or spends its budget is counted, never merely logged (AGENTS.md):

| Field | Counts | Note |
|---|---|---|
| `frames_captured` | successful `camera_fb_get()` results, in stream **and** transport | |
| `frames_delivered` | frames whose send completed | a gap against `frames_captured` is loss; use the row below to see the cause |
| `frames_send_failures` | frames whose send **started and failed part-way** because the client vanished (MJPEG chunk send, TCP `send_all`), counted in the MJPEG handler and the TCP transport | the frame is gone and is never retried |
| `frames_near_budget` | frames whose JPEG reached **≥90% of `frame_budget_bytes`** | the serial log prints the first and every 100th; the counter is the full history |
| `snapshots_served` | successful `GET /api/v1/snapshot` responses | a snapshot deliberately does **not** touch `frames_captured`/`frames_delivered`, so a client polling snapshots can never make a dead stream look alive to the liveness watchdog (15 s stalled / 30 s no-capture) |

## Frame budget and quality floor (normative)

The OV2640's JPEG size is chosen by the sensor and depends on **scene content**, not only on the quality setting. If a frame exceeds the driver's frame buffer the camera enters `cam_hal: FB-OVF` and stops producing usable frames — a state that, before ADR-0009, could only be cleared by a power cycle. The firmware therefore treats the frame buffer as a hard budget:

- `frame_budget_bytes` (262,144) comes from `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE`. The driver's AUTO mode (`width*height/5`) is **not** used: it left only 1–29% headroom at quality 4 and overflowed below it.
- `quality_floor` is the lowest quality measured to fit the budget with margin on this hardware. `/api/v1/config` **rejects** anything below it with **400** and an explicit message; the app clamps its slider to the same value.
- Measured floors (10 s streams, 44–144 frames/cell, `benchmarks/results/qfloor-20260926.json`):

| resolution | floor | max frame measured at floor | margin |
|---|---|---|---|
| QQVGA / QVGA | q0 | 59,976 B (QVGA q0) | 4.4x |
| VGA | q2 | 105,952 B | 2.5x |
| SVGA | q2 | 155,955 B | 1.7x |
| XGA | q6 | 143,673 B | 1.8x |
| HD | q6 | 167,811 B | 1.6x |
| SXGA | q8 | 201,317 B | 1.3x |
| UXGA | q16 | 98,587 B | 2.7x |

- **The residual risk is stated, not hidden:** verification runs observed up to ~2x the 10 s maximum for the same cell (UXGA q12 measured 128,659 B, later 259,471 B = 99% of budget). Floors that showed <2x margin under that variation were raised (UXGA q12 → q16). A sufficiently complex scene can still overflow; the handler then recovers automatically and the stream resumes, and a frame at >=90% of budget is counted in `frames_near_budget` and logged as a warning (first and every 100th).
- Quality has a large frame-rate cost (sensor-side JPEG encoding): 1.5–10 fps at the floors versus 19–45 fps at q12–q36. The throughput ladder is unchanged (ADR-0008).

## HTTP transport behaviour (observed, normative for clients)

Verified against firmware `0.1.0` / proto v1 on 2026-09-26.

- **`Connection: close` is not honoured.** `esp_http_server` keeps the session open after a complete response and only closes it on idle timeout (control server: `recv_wait_timeout=2` s) or LRU purge. Clients **must** frame responses by `Content-Length` (all control endpoints send it) and must not wait for EOF. A client that reads until close will always see a spurious timeout even though the response arrived complete. Only the 404 path and the MJPEG stream end with a close.
- **MJPEG framing:** `Transfer-Encoding: chunked` with `Content-Length` per part; every part is preceded by the `--FRAME` boundary. The stream ends only when the client goes away or the camera fails the frame — and at that point the multipart body is closed with the RFC 2046 close-delimiter `\r\n--FRAME--\r\n` followed by the terminating zero-length chunk of the transfer. (It previously ended abruptly between two boundaries; `MjpegClient` tolerates that, but a stricter reader stalls waiting for a boundary that never comes.)
- **Reconnect contract (desktop client, `MjpegClient`):** up to 5 attempts with 0.5/1/2/3/5 s backoff, then the attempt counter resets only when a **frame is actually decoded** — a TCP connect is not success. Two watchdogs bound every attempt: no bytes within 6 s of connect ⇒ `no response from camera`, and no data for 8 s while streaming ⇒ `stream stalled`. The UI's connected state follows the first decoded frame, not the TCP handshake, so a failing device can never masquerade as a live stream or hold the Connect button hostage. After two consecutive `no response` failures the client asks the device to re-initialise the camera through `/api/v1/config` (the self-repair path) before continuing the ladder.
- **Stream authentication posture (FW-18).** `GET /stream` (and the TCP/UDP transports) require no session token; `GET /api/v1/snapshot` and every other `/api/v1/*` route do. The split is deliberate and disclosed rather than hidden — the capabilities response reports `stream_port_protected: false`, and `docs/architecture.md` records the same posture. The control plane is closed uniformly: a snapshot is one still served on demand on a plane where a token check is already enforced for every neighbouring route, so leaving it out would open a hole in an otherwise closed plane. The video plane is a separate port with no TLS on the ESP32 (architecture → *Authentication*), so a token would travel in clear text and buy no confidentiality against a listener who can already read the frames it guards — while costing every third-party MJPEG viewer and the benchmark harness. The asymmetry is therefore a written posture decision, not an oversight. Revisit triggers: TLS on the video plane (with its measured cost/benefit), or the device being placed on an untrusted or multi-tenant network.
- The stream handler is one client per handler invocation; a second concurrent connection triggers ESP-IDF's LRU purge of the older session. One viewer per device is the supported configuration.
- **Config while streaming is rejected with 409** (see control plane). Changing resolution or quality requires an explicit disconnect first; the app stops the stream *and* the status polling, then waits (up to 8 s) for the device to report `stream_clients == 0` before sending the config, so a dying handler no longer causes a 409 loop.

