# Architecture

_Status: Phase 2 baseline implemented — firmware + desktop both build (2026-09-23); hardware bring-up pending._

## System overview

```
ESP32-CAM (OV2640, JPEG, softAP "ESP32-CAM" @192.168.4.1) ◄── Wi-Fi ── Windows Qt 6.11 app
   capture → JPEG → PSRAM → transport          (PC joins camera SSID) → receive → assemble → decode → render
```

- Topology: ESP32 runs a softAP; the PC connects to it directly — no router in the path (ADR-0006; supersedes station+router ADR-0001). Camera IP fixed at `192.168.4.1`.
- Priority: image quality > sustained FPS (≥15, prefer ≥20) > latency, with usable live view at all times.
- One camera in v1; all core types are per-instance (no global camera singleton) so multi-camera UI can be added later.

## Firmware architecture (ESP-IDF)

Modules (each a small cohesive unit under `firmware/esp32_cam_stream/main/`):

| Module | Responsibility |
|---|---|
| `camera` | esp32-camera init, JPEG-only pipeline, PSRAM framebuffers, `CAMERA_GRAB_LATEST`, runtime reconfig (resolution/quality/sensor controls) |
| `wifi` | softAP start (WPA2, fixed IP, channel/max-clients policy), connected-station RSSI, power-save policy |
| `transport` | frame delivery. Three server-pull senders, each a task/handler that fetches frames from the camera driver and reports to `metrics`: HTTP MJPEG (stream handler), TCP framed + UDP packetized (`frame_transport.c`, selected/primary per ADR-0007) |
| `control_api` | authenticated JSON control/status endpoints (HTTP) |
| `discovery` | UDP broadcast announce on 48888 (no mDNS — see Discovery) |
| `metrics` | capture/tx FPS, bytes, heap/PSRAM, RSSI counters, reported via status API + serial log (rate-limited) |
| `app` | wiring, task/core affinity, watchdogs |

Pipeline: OV2640 JPEG → PSRAM fb → (optional early send during DMA where measurable) → transport. No RGB/YUV conversion on device. No recording/transcoding on device.

### Socket budget (hard limit)

Every lwIP socket the firmware opens comes out of one pool (`CONFIG_LWIP_MAX_SOCKETS`). `esp_http_server` needs `max_open_sockets + 3` sockets **per instance** (listen + ctrl socket + 1 reserved), so the floor is structural, not empirical:

| Consumer | Sockets |
|---|---|
| control httpd (port 80, `max_open_sockets=4`) | 7 |
| stream httpd (port 81, `max_open_sockets=2`) | 5 |
| raw TCP/UDP frame transport (`CONFIG_SORTING_CAM_FRAME_TRANSPORT`, default **off**) | 2 |
| worst case with transport enabled | 14 |
| `CONFIG_LWIP_MAX_SOCKETS` | **16** |

Consequences that are now enforced by review, not luck:

- The pool must exceed the worst case with headroom. Undersizing it does not degrade gracefully: `accept()` starts failing with `ENFILE` (errno 23), the listen socket stays readable, and the httpd task busy-spins at 100% CPU while new connections rot in the backlog — clients see the connection closed or stalled, and the extra CPU load shows up as brownout risk on USB power.
- Anything that permanently holds a socket (the raw transport) must be opt-in. It is off by default because no shipped client uses it.
- Churn matters: rapid connect/abort cycles were what pushed the pool to exhaustion. `CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT` is 5 s (default 20 s) so closed sessions release their PCB quickly, and the control server uses `recv_wait_timeout=2` so idle keep-alive sessions are dropped fast.
- Verified 2026-09-26 after the fix: 20 rapid connect/abort cycles plus interleaved config changes and status polling produced zero `error in accept` entries, and every session streamed.

## Camera failure model and recovery

Three independent recovery layers, because a wedged OV2640 is otherwise a brick that only a power cycle clears:

| Layer | Trigger | Action | Cost to the viewer |
|---|---|---|---|
| Frame-budget guard | config with quality below the measured floor | **400**, nothing is touched | none — request refused |
| Capture recovery (stream/snapshot handler) | `camera_fb_get()` returns NULL (driver frame timeout, e.g. after an overflow) | `camera_recover()`: deinit → PWDN power-cycle → SCCB bus recovery → init; up to 2 attempts, then the client is closed | the stream pauses ~1–5 s and resumes on the same connection |
| Watchdogs (`app_main.c`) | delivery stalled 15 s with a client attached, or no frame captured for 30 s with a client attached | `camera_recover()`; `esp_restart()` only if recovery itself fails | stream resumes, or a reboot if the sensor is unrecoverable |

Design constraints this imposes:

- **The stream handler must never block indefinitely.** It runs inside the single-threaded stream httpd task, so anything that waits there stops *all* new stream connections. Capture failures therefore recover immediately instead of retrying 50 times (which cost ~225 s and, in the field, looked exactly like "the camera stopped working").
- The camera mutex is held across a frame's whole lifetime (`camera_fb_get` takes it, `camera_fb_return` releases it) so a config apply cannot `esp_camera_deinit()` underneath a frame in flight. Every caller must return every frame or the whole camera wedges.
- The operating point (framesize, quality, fb_count, grab mode, fb location, xclk) is persisted in NVS (`camcfg`/`v1`, CRC-checked) and restored before the first driver init, so a watchdog reboot returns to the user's chosen settings instead of silently reverting to the compiled defaults.

## Operating profiles and their frame-rate floors

Production default: **1280×720 (HD)**, JPEG quality 12, XCLK 18 MHz, fb_count 3,
`CAMERA_GRAB_LATEST`, PSRAM frame buffers — `CAM_DEFAULT_*` in `app.h`, restored
from NVS after a reboot. The floors below are per profile, not one global
number: the §34 "sustained minimum 15 FPS" target is **not reachable at 1280×720**
on this hardware (see `docs/benchmark-results.md` "Gate arithmetic" and ADR-0010).

| profile | quality | XCLK | measured fps (120 s, app path) | profile floor | vs 15 fps target |
|---|---|---|---|---|---|
| **HD 1280×720 (default)** | q12 | 18 MHz | 11.01 best, 7.74–11.17 band across 5 runs | **≥7**, provisional pending the 1 h soak | not met — hardware ceiling ≈11.2 |
| HD 1280×720 | q24 | 18 MHz | 11.17 | ≥7, provisional | not met |
| SVGA 800×600 | q36 | 18 MHz | 22.42 | **≥20** | met, +12% |
| SVGA 800×600 | q36 | 27 MHz | 33.62 | ≥30 | met, +124% |
| VGA 640×480 | q36 | 18 MHz | 22.51 | **≥20** | met, +13% |
| QVGA 320×240 | q24 | 18 MHz | 44.94 | **≥40** | met, +200% |

Rules that follow from the measurements, not from preference:

- **fps is the camera's frame-production rate.** The link peaks at 4.36 Mbps
  observed and the PC consumes every delivered frame with zero drops, so neither
  is a lever for frame rate.
- **Per-profile clock.** XCLK is not global: 27 MHz is the best measured point at
  SVGA (+50% over 18 MHz) and produces **no frames at all** at HD. The firmware
  enforces the measured ceiling per resolution (`camera_xclk_max_mhz`) and the app
  must send the clock with the profile, not once for the session.
- **HD is byte-rate limited, so its frame rate moves with scene complexity**
  (7.7–11.2 fps tracking 58 KB → 45 KB frames). The frame interval is linear in the
  compressed frame size at ~2.2 ms per KiB (correlation −0.85 across seven runs at
  one fixed configuration), because the OV2640 encodes in the sensor and encode
  time scales with the data emitted; the driver paces frames on the sensor's
  end-of-frame, so `camera_fb_get` returns in 0.4–6 ms while frames arrive every
  90–140 ms. A hard ceiling near 11.2 fps also exists at 1280×720 (seen as the
  maximum across q12 and q24 and by the Phase 4 harness). HD floors are therefore
  stated as a band and will be tightened to the 1 h soak's sustained figure.
- Image quality is compared per pixel, not per file: HD q24 and SVGA q36 both
  encode 0.033 B/px, so SVGA is 3× the frame rate at equal per-pixel quality.

Turning these into an adaptive profile selector (§12) is Phase 6 work — camera
control and configuration — not part of this phase.

## Desktop architecture (Qt 6.11, LGPL modules)

| Layer | Types | Thread |
|---|---|---|
| UI | QML (Qt Quick, RHI/D3D11 on Windows) | GUI thread only presents |
| Device mgmt | `DeviceModel`, `DiscoveryService` (mDNS + UDP), `Device` (stable ID ≠ DHCP IP) | control thread |
| Session | `CameraDevice`, `Connection`, `StreamSession` (one per camera) | net thread per session |
| Transport | `ITransport` ← `HttpMjpegTransport`, `TcpFramedTransport`, `UdpJpegTransport` | net thread |
| Pipeline | `FrameAssembler` → `FrameSink` (latest-wins, bounded) | net thread |
| Decode | `IframeDecoder` (start: QImage) | decode thread |
| Render | `FrameProvider` (QQuickRhiItem / SceneGraph texture path) | render thread |
| Record | `Recorder` (original JPEG bytes + index) | record thread |
| Metrics | `MetricsRegistry` (counters, rolling stats) | lock-free/atomic counters, polled by UI timer |

Rules: GUI thread never blocks on network or decode; no QLabel/QPixmap video path; digital zoom/pan in render path only; snapshot/recording always use original JPEG payload.

## Protocol candidates (Phase 3 decides)

1. HTTP multipart MJPEG (baseline; simplest, proven)
2. TCP custom framed JPEG (versioned header, see `docs/protocol.md`)
3. UDP packetized JPEG (fixed-MTU chunks, loss → frame drop, candidate for latency)

Selection = measured quality/FPS/latency + stability veto. Control API runs over HTTP regardless of video transport.

## Discovery

**Implemented in Phase 6 as UDP broadcast only.** mDNS was planned as the
primary and UDP as the fallback; it was not built, because the softAP profile
(ADR-0006) puts the camera and PC on a link where broadcast already works
reliably and mDNS would add a Bonjour dependency on Windows for no measured
gain. The hybrid design stays a candidate only if a station-mode profile ever
needs to cross subnets.

| | |
|---|---|
| Query | PC broadcasts `{"scam":1,"op":"discover"}` to port **48888** every 2 s |
| Reply | Unicast announce from the camera, rate-limited to one per 250 ms |
| Announce | `device_id` (MAC-derived, stable across DHCP changes), `device_name`, `ip`, `fw_version`, `proto_version`, `sensor`, `control_port`, `stream_port`, `auth_required`, `resolutions[]`, `controls{}` grouped by function |
| PC side | `DiscoveryService` is a `QAbstractListModel` on its own thread; dedupes by **device id, never IP**, ages an entry out after 12 s of silence, falls back to the sender address if an announce omits its own IP |
| UI | Device picker in the toolbar replaces manual IP entry as the primary path; manual entry is kept as a fallback. The first discovered device is auto-selected while unconnected |

The stable device id is what keeps §19 authentication intact across a DHCP
change: credentials are keyed by device id, so a new IP does not orphan a
stored password. Manual IP entry remains for a device that cannot announce.

## Connection state machine (§24)

`SessionState` derives one token from three independent observers — discovery,
control plane, video transport — so the UI reads a single value instead of
inferring meaning from a pile of booleans. The eight states named in §24 are
the contract:

| token | meaning |
|---|---|
| `discovering` | nothing connected, UDP search in progress |
| `connecting` | first stream attempt in flight |
| `authenticated` | control API answering, video not started |
| `streaming` | video and control both healthy |
| `degraded` | video running but something is wrong: frames stalled, control unreachable, sign-in still required, or the last setting was rejected |
| `reconnecting` | inside the bounded retry ladder, with attempt/delay shown |
| `disconnected` | not connected and not searching |
| `error` | retries exhausted, or credentials rejected |

The derivation lives in the pure function `SessionState::derive(SessionInput)`,
which takes a plain struct and returns a plain struct. That is deliberate: the
whole table is unit-testable without a network, a camera, or a Qt event loop —
see `tests/tst_sessionstate.cpp` (18 checks), including the ordering rules that
matter most:

- an exhausted retry ladder **outranks** a still-reachable control plane, so a
  stale error can never let the UI claim the camera is fine;
- reconnecting **outranks** authenticated;
- authenticated **outranks** discovering — discovery runs continuously, so
  without this rule the token would never stop saying "searching";
- "sign in required" reports as `warn`, not `err`, because the user can clear
  it by acting; rejected credentials report as `err`.

Reconnection itself is bounded in `MjpegClient`: 5 attempts at
0.5/1/2/3/5 s, counters reset only on a **decoded frame** (a TCP connect is not
success), two watchdogs bound every attempt (6 s to first byte, 8 s stalled).
This is the "no reconnect storms" requirement of §24.

## Authentication (control plane only)

- Session token issued after challenge–response login (HMAC of server nonce + password hash); token required on all control endpoints.
- Rate-limit + lockout on failures; no plaintext creds in logs; NVS-stored password hash (salted); replay protected via nonce + timestamp window.
- Video stream itself unauthenticated on trusted LAN for now (ADR; revisit if product scope changes). No TLS on ESP32 without measured cost/benefit.

## Recording & snapshot

_PC-side only. The ESP32 performs no recording and no transcoding._

Both features consume the **exact JPEG bytes the camera sent**, never a
decoded-and-re-encoded copy (§21, §22). The pipeline was changed to make that
possible: `MjpegWorker` used to hand over only a decoded `QImage`, which
discarded the payload at `image.loadFromData()`. It now passes the untouched
`payload` alongside the image, and `FrameBus` holds both views of the same
frame — so a snapshot is always the picture on screen, and the recorder always
gets the network bytes.

| | |
|---|---|
| Snapshot | current frame's bytes straight to `<Pictures>/SortingCamera/snapshots/snap_<stamp>_<device>.jpg`, size verified against what was written. Exposed to QML only as `save(directory, deviceId)`; the bytes stay in C++ |
| Recording | `.scamrec` container: magic + JSON header, then `u32` length + raw JPEG per frame, plus a `.json` sidecar index with `offset/seq/ts/w/h`. Written at `<Pictures>/SortingCamera/recordings/` |
| Index | flushed at most every 60 frames and again on a clean stop. It is a convenience, not a dependency |
| Recovery | `Recorder::scan` rebuilds the frame table from the container alone: read the length, check it is plausible, check the bytes are a well-formed JPEG, stop at the first failure — which is where a truncated write ends. A killed application costs the frames since the last flush, never the file |

Format selection, with the four candidates evaluated against §21's six
criteria: `decisions/0011-recording-container-format.md`.


## Threading summary

Net I/O+assembly, decode, record per stream; discovery/control single thread; metrics via atomics polled at 1–2 Hz; GUI/scene-graph threads Qt-owned. Threads created only where workload justifies.

## Risks / open questions

- OV2640 system-level FPS ceiling: sensor datasheet UXGA ≤15 fps; peer ESP32-CAM study measured ~1.3 fps UXGA and ~14 fps VGA over HTTP — exact operating envelope TBD by our Phase 4 matrix (never assume).
- Windows mDNS reliability → avoided entirely by using UDP broadcast, which needs no name service.
- Decode path: QImage baseline; FFmpeg/Multimedia only if measured need.
