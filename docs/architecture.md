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
| `discovery` | mDNS + UDP broadcast announce |
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

**Deferred while the fixed-IP softAP baseline stands (ADR-0006)** — the camera is always at `192.168.4.1`. Planned hybrid design for a future multi-camera/station profile: mDNS `_http._tcp` / custom service type (primary) + UDP broadcast probe on fixed port (fallback, works without Bonjour on Windows). Announce: stable device ID, name, fw version, proto version, ports, resolution capabilities.

## Authentication (control plane only)

- Session token issued after challenge–response login (HMAC of server nonce + password hash); token required on all control endpoints.
- Rate-limit + lockout on failures; no plaintext creds in logs; NVS-stored password hash (salted); replay protected via nonce + timestamp window.
- Video stream itself unauthenticated on trusted LAN for now (ADR; revisit if product scope changes). No TLS on ESP32 without measured cost/benefit.

## Recording & snapshot

- PC-side only. Store received JPEG bytes verbatim.
- Container: indexed frame stream (length-prefixed frames + JSON sidecar index with seq/timestamp/dims/device/firmware) — chosen in Phase 6 after format evaluation; crash-recoverable by scanning markers.
- Snapshot = current frame's exact bytes to `.jpg`.

## Threading summary

Net I/O+assembly, decode, record per stream; discovery/control single thread; metrics via atomics polled at 1–2 Hz; GUI/scene-graph threads Qt-owned. Threads created only where workload justifies.

## Risks / open questions

- OV2640 system-level FPS ceiling: sensor datasheet UXGA ≤15 fps; peer ESP32-CAM study measured ~1.3 fps UXGA and ~14 fps VGA over HTTP — exact operating envelope TBD by our Phase 4 matrix (never assume).
- Windows mDNS reliability → hybrid discovery mitigates.
- Decode path: QImage baseline; FFmpeg/Multimedia only if measured need.
