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
