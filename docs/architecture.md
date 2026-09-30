# Architecture

_Status: living document. Phase 2 baseline landed 2026-09-23; Phases 3–6 are complete and their measurements live in `docs/benchmark-results.md`; the Phase-6/7 audit remediation is in Stage 4 as of 2026-09-30 (every finding's status in `docs/audits/PHASE7_REMEDIATION.md`), with hardware bring-up and the Phase 7/8 gates still pending._

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
| `transport` | frame delivery. Three server-pull senders, each a task/handler that fetches frames from the camera driver and reports to `metrics`: HTTP MJPEG (stream handler) — **the one that ships**; TCP framed + UDP packetized (`frame_transport.c`, compiled in but never started: `CONFIG_SORTING_CAM_FRAME_TRANSPORT` defaults to `n`, and enabling it exceeds the socket budget). ADR-0007 chose TCP as primary and was **never implemented** (DD-1; amendment in the ADR) |
| `control_api` | authenticated JSON control/status endpoints (HTTP) |
| `discovery` | UDP broadcast announce on 48888 (no mDNS — see Discovery) |
| `metrics` | capture/tx FPS, bytes, heap/PSRAM, RSSI counters, plus lost/near-budget frames and snapshots served — exported through the status API and mirrored in the rate-limited serial log |
| `app` | wiring, task/core affinity, watchdogs |

The table names logical modules; the code has files, and the two vocabularies
have to map onto each other or the table rots (DD-6): `camera` → `camera.c`
(+ `camera_control.c` for the sensor controls), `wifi` → `wifi.c`, `transport`
→ `http_servers.c` (the MJPEG stream handler) plus `frame_transport.c` (compiled
in, started only by Kconfig), `control_api` → `http_servers.c`, `discovery` →
`discovery.c`, `metrics` → `metrics.c`, `app` → `app_main.c`, and authentication
→ `auth.c`.

Pipeline: OV2640 JPEG → PSRAM fb → (optional early send during DMA where measurable) → transport. No RGB/YUV conversion on device. No recording/transcoding on device.

### Socket budget (hard limit)

Every lwIP socket the firmware opens comes out of one pool (`CONFIG_LWIP_MAX_SOCKETS`). `esp_http_server` needs `max_open_sockets + 3` sockets **per instance** (listen + ctrl socket + 1 reserved), so the floor is structural, not empirical:

| Consumer | Sockets |
|---|---|
| control httpd (port 80, `max_open_sockets=6`) | 9 |
| stream httpd (port 81, `max_open_sockets=2`) | 5 |
| discovery UDP (port 48888) | 1 |
| **baseline (the desktop and benchmark path)** | **15** |
| raw frame transport (`CONFIG_SORTING_CAM_FRAME_TRANSPORT`, default **off**): TCP listener + accepted client + UDP | 3 |
| worst case with transport enabled | **18** |
| `CONFIG_LWIP_MAX_SOCKETS` | **16** |

Consequences that are now enforced by review, not luck:

- The pool must exceed the worst case with headroom. Undersizing it does not degrade gracefully: `accept()` starts failing with `ENFILE` (errno 23), the listen socket stays readable, and the httpd task busy-spins at 100% CPU while new connections rot in the backlog — clients see the connection closed or stalled, and the extra CPU load shows up as brownout risk on USB power.
- The baseline already sits at **15 of 16**: one free socket, exactly as intended for a single client. The raw transport **does not fit the pool at all** (18 > 16) — it is not a "raise it a bit" option but a configuration change, which is why it stays default-off.
- Anything that permanently holds a socket (the raw transport) must be opt-in. It is off by default because no shipped client uses it.
- Churn matters: rapid connect/abort cycles were what pushed the pool to exhaustion. `CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT` is 5 s (default 20 s) so closed sessions release their PCB quickly, and the control server uses `recv_wait_timeout=2` so idle keep-alive sessions are dropped fast.
- Verified 2026-09-26 after the fix: 20 rapid connect/abort cycles plus interleaved config changes and status polling produced zero `error in accept` entries, and every session streamed.

## Camera failure model and recovery

Three independent recovery layers, because a wedged OV2640 is otherwise a brick that only a power cycle clears:

| Layer | Trigger | Action | Cost to the viewer |
|---|---|---|---|
| Frame-budget guard | config with quality below the measured floor | **400**, nothing is touched | none — request refused |
| Capture recovery (stream/snapshot handler) | `camera_fb_get()` returns NULL (driver frame timeout, e.g. after an overflow) | `camera_recover()`: deinit → PWDN power-cycle → SCCB bus recovery → init; up to 2 attempts, then the client is closed | the stream pauses ~1–5 s and resumes on the same connection |
| Watchdogs (`app_main.c`) | delivery stalled 15 s with a client attached, or no frame captured for 30 s with a client attached | stall: `camera_recover()`, which re-arms the 15 s window; no-capture: `camera_recover()` at most `CAMERA_NO_CAPTURE_RECOVERY_LIMIT` (3) times, one full 30 s window apart, then the camera is left down and said so; `esp_restart()` only if a recovery itself fails | stream resumes, a bounded number of power-cycles, or a camera left down that `/status` still reports |

Design constraints this imposes:

- **The stream handler must never block indefinitely.** It runs inside the single-threaded stream httpd task, so anything that waits there stops *all* new stream connections. Capture failures therefore recover immediately instead of retrying 50 times (which cost ~225 s and, in the field, looked exactly like "the camera stopped working").
- **The camera lock is a drain gate, not a hold-for-life mutex.** A frame buffer may
  leave the driver and be held by a caller, but `s_gets_in_flight` / `s_fb_held`
  (`camera.c`) are counted under `s_cam_mutex`, and a teardown takes that mutex,
  sets `s_gate_closed`, and polls until both are zero before it calls
  `esp_camera_deinit()` — which frees the buffers. Consumers take their buffer with
  the lock released (so a slow client delays only the teardown, never any other
  camera user) and wait with the lock released if the gate is closed; they give it
  back under the lock. The drain is bounded by `CAM_TEARDOWN_TIMEOUT_MS` (7 s, a
  little over the 5 s socket send timeout every path has); on timeout the gate is
  reopened, the driver is left alone, and the teardown reports
  `ESP_ERR_INVALID_STATE`. Every caller must return every frame or the drain never
  completes, and **a caller must never hold a frame buffer while it asks for a
  teardown** — it would wait for itself. See ADR-0016.
- The operating point (framesize, quality, fb_count, grab mode, fb location, xclk) is persisted in NVS (`camcfg`/`v1`, CRC-checked) and restored before the first driver init, so a watchdog reboot returns to the user's chosen settings instead of silently reverting to the compiled defaults.
- **A watchdog must not be able to power-cycle the sensor at 1 Hz (FW-8).** The no-capture branch used to call `camera_recover()` on every 1 s tick, because the clock it reads (`metrics_us_since_last_capture`) is advanced only by a real capture — so a camera that re-initialised cleanly but never delivered a frame was reset forever, silently. Recovery on that branch is now bounded and spaced: at most 3 attempts, one `CAMERA_DEAD_TIMEOUT_US` window apart, then the camera is **left down with an error log**. Leaving it down is the decision: the control plane stays up, `/status` still reports `camera_up=0` and `camera_recoveries`, and a config apply re-runs the recovery path, so a camera that stays dead is something an operator can see and act on rather than watch rebooting a thousand times.

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

### As built

The table below is what the code actually contains, not a target. Phase 3
selected HTTP multipart MJPEG (ADR-0007), so the TCP-framed and UDP
transports below the baseline are candidates that were measured and rejected,
not code.

| Layer | Type (as built) | Thread | Notes |
|---|---|---|---|
| UI | `desktop/qml/Main.qml` | GUI thread, presentation only | never does network I/O or JPEG decode |
| Discovery | `DiscoveryService` (UDP :48888) | own QThread | dedupes by device id, not address; 12 s ageing |
| Registry | `DeviceRegistry` | GUI thread | owns every `CameraDevice`; decides which is active |
| Session | `CameraDevice` (ADR-0013) | GUI thread, owns the rest | one per camera, knows nothing of any other |
| Control | `DeviceStatus` → `AuthClient` → `CredentialStore` | own QThread | DPAPI-backed credentials, Bearer + 401 retry |
| Transport | `MjpegClient` | net thread per camera | chunked multipart parser, latest-wins |
| Pipeline | `FrameBus` (image **and** raw bytes) | net thread writes, GUI reads | one latest frame, no queue |
| Decode | `MjpegClient` (in the net thread) | net thread | feed-and-run, 19–36 % of one core measured |
| Render | `FrameImageProvider` (`image://frame/live`) | render thread | resolves the **active** bus per request |
| Record | `Recorder` (`.scamrec`, ADR-0011) + `SnapshotWriter` | net thread | exact received JPEG bytes, never re-encoded |
| Metrics | `AppMetrics` (bench) + `StreamStats` (live) | atomic / 1 s timer | `Diagnostics::Facility` counters for the rest |
| Profiles | `ProfileEngine` (ADR-0012) | GUI thread | measured ladder + automatic mode |
| Notices | `NotificationCenter` | GUI thread | shares the `Diagnostics` severity vocabulary |
| Prefs | `UserPreferences` (QSettings) | GUI thread | §37 layer, separate from credentials |

Rules that hold: the GUI thread never blocks on network or decode; the
display-path zoom never touches the frame in `FrameBus`; snapshot and recording
always use the original JPEG payload; the bus is bounded to one latest frame
with every drop counted.

### Device model (ADR-0013)

`DeviceRegistry` is the only place that knows how many cameras exist. It hands
out one `CameraDevice` per camera and tracks which is active; it holds no
transport, no decoder and no view. Devices are keyed by the camera's own device
id and never by address — a camera that answers on a new address keeps its
session, credential and recording target. Acquiring is not selecting: an
announce for a second camera must not pull the view away from the one being
watched, and `releaseStale()` never collects the active device.

The view is single-camera, which §27 permits. Adding a second simultaneous view
is future work and is **not** claimed.

### Threading

| Thread | Owns | Reads from others |
|---|---|---|
| GUI | `FrameBus` (read), `NotificationCenter`, `ProfileEngine`, `SessionState` | `FrameBus` snapshot, atomic counters |
| net (per camera) | `MjpegClient` decode, `Recorder` append | socket |
| control (per camera) | `DeviceStatus`, `AuthClient` | HTTP |
| discovery | `DiscoveryService` worker | UDP socket |
| render | — | `FrameBus` via the image provider |

`QHostAddress` is a `Q_GADGET` and cannot cross a queued connection, so the
discovery worker passes the sender as a `QString` and resolves it on the
receiving side. Passing it directly delivers nothing and then faults.

**Teardown is synchronous and bounded (DX-17).** `CameraDevice` is destroyed on
the GUI thread, and with it `MjpegClient` and `DeviceStatus`, whose destructors
stop their worker with a `BlockingQueuedConnection` followed by `QThread::wait()`.
That structure is deliberate — the worker owns the socket, the timers and the
parser, so it must be stopped before the object goes away — and what makes it
acceptable is that the wait is bounded: `stop()`/`stopPolling()` only stop timers
and abort a socket, so the caller waits for the event handler already in progress
and nothing more. Measured 2026-09-30 with frames still in flight: **2 ms**
(`MjpegClient`) and **1 ms** (`DeviceStatus`). `tst_capture` and
`tst_devicestatus` assert a **200 ms** ceiling — the point at which a block like
this stops being invisible and starts reading as a frozen UI — so the bound is a
tested claim rather than a comment. Moving teardown off the GUI thread stays
future work for the day a measurement ever crosses that ceiling.


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
  - **Why snapshot is checked and the stream is not (FW-18):** `GET /api/v1/snapshot` sits on the control plane behind the same token as every other `/api/v1/*` route, and removing it would be a hole in a plane that is otherwise closed. The stream lives on its own port with no TLS, so a token would only be sent in clear text — no confidentiality gained, third-party viewers and the benchmark harness broken. The split is advertised honestly as `stream_port_protected: false` in the capabilities response; the full rationale and revisit triggers (TLS on the video plane, untrusted/multi-tenant network) are in `docs/protocol.md`.

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

See the desktop threading table above — it lists the threads that exist and what
each owns. In short: network I/O, decode and record share one thread per camera;
the control channel and discovery each have their own; metrics are atomic
counters plus a 1 s timer; the GUI and scene-graph threads are Qt-owned. Threads
exist only where the workload justified one — decode is **not** on a separate
thread, because feed-and-run inside the network thread measured 19–36 % of one
core and a hand-off cost more than it saved.

## Hardware capability detection (§28)

The final HMI hardware is unknown (§28), so the app reads the machine's
capabilities at startup instead of assuming the dev box. `Capabilities`
(`desktop/src/Capabilities.{h,cpp}`, library-side so a test can link it) answers
the eight bullets:

| §28 bullet | Probe | Fallback when it cannot be measured |
|---|---|---|
| CPU characteristics | Windows `ProcessorNameString` registry value, `QSysInfo::buildCpuArchitecture()`, `QThread::idealThreadCount()` | architecture only, core count reported as `unknown` |
| available memory | `GlobalMemoryStatusEx` | `unknown (not probed on this platform)` |
| graphics backend | `QGuiApplication::platformName()` + `QQuickWindow::sceneGraphBackend()` once a window exists (`graphicsApi()` before that) | `default` — it does not invent a name it does not have |
| graphics acceleration | the backend above, plus the documented `QT_QUICK_BACKEND=software` override | software backend → *software fallback*, `hardwareGraphics = false` |
| video decode acceleration | none exists to probe | stated as **none**: JPEG is decoded by Qt on the CPU, and that software path is the fallback §28 asks to keep safe |
| display resolution | `QGuiApplication::primaryScreen()`: geometry, refresh rate, DPI, manufacturer/model | `unknown (no screen attached)` |
| network interface | `QNetworkInterface::allInterfaces()` filtered to up, non-loopback, IPv4 | `no IPv4 interface is up` |
| touch capability | `QInputDevice::devices()` (`TouchScreen`/`TouchPad`) and `QPointingDevice::primaryPointingDevice()` | `none detected (mouse/keyboard only)` |

Nothing branches on these values **yet**. §28's "use capability-based behavior"
is a later step, deliberately: while the probe is reporting-only, a probe that is
wrong about an exotic machine cannot change what the app does, only what the
panel says. The list is re-probed when the F12 diagnostics panel opens — which is
also the first moment the scene-graph backend name is real — and two derived flags
travel with it: `hardwareGraphics`, and `onCameraAccessPoint` (some interface holds
`192.168.4.x`, i.e. the PC has joined the camera's softAP, which is the most common
reason a viewer shows nothing). Covered by `tst_capabilities`.

## Diagnostics (38, ADR-0014)

`Diagnostics::Facility` is one per process and installs a Qt message handler, so
ordinary `qDebug()`/`qInfo()` calls are captured at the level Qt assigned and
with a category taken from the enclosing `Diagnostics::Scope`. Five levels
(DEBUG, INFO, WARNING, ERROR, CRITICAL) and one category per subsystem that §38
names.

- **Rate limiting is per level, per window.** CRITICAL is unlimited.
- **Suppression is counted, never dropped.** The count rides on the next line
  that gets through, so a log that swallowed 97 retries still answers "how often
  does this fail" correctly. `setRateLimit()` preserves the pending count when a
  limit changes at runtime.
- **High-rate quantities are counters, not lines:** frame bytes, frame age,
  frames received, drops (as a delta, not a running total). Each keeps
  min/max/spread, so a steady stream is distinguishable from a stuttering one
  without logging anything.
- The diagnostics view is **on demand (F12)**, not always visible.

An unannotated call site lands in `other` rather than nowhere, so it shows up as
unattributed instead of disappearing.

## Notifications (26)

`NotificationCenter` is a list model per camera, newest first, fed by the session
state machine, the recorder and the control channel. It shares the
`Diagnostics` severity vocabulary so a condition cannot be a warning in the log
and an error on screen.

- **Info expires** (8 s default) — nobody needs to read twice that the camera was
  found.
- **Warning and above are sticky** until dismissed — nobody has acknowledged an
  error until they act on it. The badge counts these.
- **`postOnce()` dedupes per key.** A flapping link cannot bury the list, and
  one repeating fault cannot hide another.

## User preferences (37)

`UserPreferences` holds only choices a person made: address, ports, last device
id, resolution, quality, capture directory, theme. It is deliberately a separate
class from `CredentialStore` — preferences are plain text a user may edit,
credentials are DPAPI-protected blobs, and mixing them would put a password one
key away from a host name. Writes are deduplicated against the stored value and
`sync()`ed immediately, so a crash cannot lose the address that would otherwise
cost a manual reconnect.

## Visual design (25)

`qml/Theme.qml` is a singleton and the single owner of every design token: the
colour palette, the type scale, the spacing steps, the interaction metrics and
the motion durations. `Main.qml` binds to it and paints no literals of its own —
it went from 105 hard-coded colours across 16 values, and 21 repeated
`font.family` declarations, to zero. That is what makes the light/dark switch
one property rather than a second stylesheet, and it is why adding a control
does not mean choosing colours again.

Two decisions are worth recording because both were found the hard way:

- **`QQuickStyle::setStyle("Basic")` in `main.cpp`.** The default Windows style
  paints its own colours and ignores `QPalette` entirely, so the first light
  build switched the hand-painted panels and left every combo box, slider and
  text field dark. `Main.qml` then maps the Theme tokens onto the standard
  palette *roles*. Only roles exist as QML properties — `palette.disabledText`
  and friends do not, because "disabled" is a group rather than a role, and
  naming one fails the whole component load at startup.
- **The dark palette is the one the application already shipped with.** The
  tokens are the pre-existing values, so introducing the file changed no
  pixels; the light palette is new.

`touchTarget` (44 px) is applied to the interactive controls rather than left
as a constant, per §25's requirement that targets be large enough for a future
touchscreen HMI. The 34 px diagnostics/footer bar is deliberately excluded —
44 px targets do not fit there without the bar overflowing.

Motion is limited to opacity on two things: a notification card fading in, and
the settings panel arriving and leaving. Both are short (90 / 160 ms) and the
panel's `visible` binding is untouched, because that binding is what keeps the
panel available at the moment a user needs it.

### Accessibility (section 25, gap G-12)

Every interactive control in `Main.qml` declares `Accessible.role` and
`Accessible.name`, plus `Accessible.description` where the visible label is an
abbreviation ("×", "Use") or where the tooltip already carries what a sighted
user takes from context. Two rules keep it honest:

- **The name says what the control does, not what it looks like.** The
  notification close button is *Dismiss notification*, not `×`; the capture
  folder's button is *Use this folder as the capture directory*, not `Use`.
- **Role values come from `QAccessible::Role`.** An unknown value is not a
  compile error: QML logs `Unable to assign [undefined] to QAccessible::Role`
  once and the item keeps no role — which is how `Accessible.Image` was found
  and rejected (the role for a picture is `Graphic`).

Dynamic evidence: `qmllint` on `Main.qml` is unchanged by the sweep (6
pre-existing layout warnings, exit 0) and the app starts clean, with no
`QAccessible` or binding errors. The durable guard is `tst_accessibility`: it
reads `Main.qml` as source and fails if a control loses its name or role —
scoped to that control's own block, so one control cannot pass on its
neighbour's annotation — if a role value is outside the known set, if a surface
(the two panes, the video image, the capability list) goes unnamed, or if the
count of named surfaces drops below the one this UI is known to have. Both
seeded mutations (delete a name, use `Accessible.Image`) are caught.

## Risks / open questions

- **The HD frame-rate ceiling is a hardware fact, not a gap.** 1280×720 tops out
  near 11.2 fps; reaching 15 needs +34 % and the only measured lever (XCLK) is
  past its safe limit. ADR-0010 scoped HD's floor to ≥7 provisionally. Profiles
  below 800×600 meet the general ≥15 / preferred ≥20 targets (ADR-0012).
- **The frame rate at a fixed configuration is scene-dependent, not a single
  number.** Measured 2026-09-29: 11.14 fps at 27,971 B/frame and 7.99 fps at
  55,792 B/frame at the same HD/q12 settings with nothing reconfigured in
  between. The bracketed measurement puts the camera itself at 8.19 frames
  captured / 8.17 delivered, so nothing between sensor and screen is the limit.
  Full numbers and the measured/correlation/hypothesis split are in
  `benchmark-results.md`. Two open items: the owner's 4–5 fps report was not
  reproduced and would need ~90–110 KiB frames, a size no run has recorded at
  HD/q12; and the cause of the scene change is uncontrolled. A deliberate
  still-scene / changed-scene A/B is the missing experiment.
- **The ≥1 h soak is still open.** All current stability evidence is from runs of
  120 s, one 4.9 min partial and one aborted attempt. Nothing here certifies
  long-duration stability, and the final endurance test must be designed around
  the finished architecture rather than repeating the current procedure.
- Windows mDNS reliability → avoided entirely by using UDP broadcast, which needs
  no name service.
- Decode path: QImage baseline; FFmpeg/Multimedia only if measured need.
- A second simultaneous camera **view** is not implemented (§27 permits one active
  stream in the UI; the objects behind it are already per camera).
