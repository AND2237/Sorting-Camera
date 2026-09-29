# Phase 6 Audit and Phase 7/8 Handoff — Sorting_Camera

_Date: 2026-09-29. Audited commit: `f3258d5` (branch `master`, tag `v0.2.0` + 4 commits).
Author: independent senior reviewer. No source file was modified by this audit._

---

## 1. Executive Summary

The repository is in far better shape than its documentation claims, and in a worse
place in one narrow area than its documentation admits.

**What is genuinely strong.** The firmware recovery model (ADR-0009) is a real
engineering artifact, not a story: the 256 KiB frame budget, the measured quality
floors, the PWDN/SCCB recovery ladder and the NVS-persisted operating point are all
present in source and match the ADRs. The threading and ownership discipline on the
desktop is better than `docs/architecture.md` describes — I verified that no blocking
network I/O, no synchronous decode and no GUI-thread mutex wait exists anywhere. The
`FrameBus` is correctly mutex-guarded, correctly latest-frame-wins, and correctly
counts drops. The reconnect ladder matches its documented contract exactly, including
the subtle part (counters reset only on a *decoded* frame, never on TCP connect). The
theme layer is a genuine design system: 241 token references, zero hard-coded colours,
`pragma Singleton` correctly wired through `QT_QML_SINGLETON_TYPE`. The release zip is
runnable on a machine with no Qt installed.

**The five findings that matter most.**

1. **A fresh clone following the documented commands builds zero tests.** `BUILD_TESTING`
   is guarded in `desktop/CMakeLists.txt:90` but `include(CTest)` appears nowhere in the
   repository, so CMake never defines it. I proved this both ways: the documented
   configure produces **0** `tst_` targets and no `CTestTestfile.cmake`; adding
   `-DBUILD_TESTING=ON` produces **60**. Every "12/12 suites green" claim in the repo
   depends on an undocumented flag. This is the single highest-leverage defect in the
   project because it silently gates the other 11 suites.

2. **A benchmark data point in the shipping product is borrowed from a different
   configuration.** `ProfileEngine.cpp:92` gives the Balanced (svga/q24) profile
   `medianBytes = 14680.0`. I searched every raw artifact: 14,680 B is the p50 of
   **vga/q24**, not svga/q24. The real svga/q24 measurements are 16,982 B (envelope,
   fb3) and 23,077 B (confirm, fb2). `ProfileEngine.h:46-49` states in its own comment
   that "putting a VGA number on an SVGA profile would be a fabricated data point" —
   and line 92 does exactly that, two hundred lines below the warning.

3. **Recording runs on the GUI thread and degrades quadratically.**
   `MjpegClient` is never moved to its own thread, so `frameReady` is a queued
   connection that lands on the GUI thread, where `Recorder::appendFrame` runs
   (CameraDevice.cpp:170-178). Every 60 frames it rebuilds the *entire* index as JSON
   and commits a `QSaveFile` — O(N) work, O(N²/60) total, on the GUI thread. A
   10-minute 45 fps recording ends with a ~27,000-entry index build every 1.3 s.
   This directly violates AGENTS.md's "GUI thread never blocks on network I/O or
   decode" — and the recorder is the one subsystem where the user is actively
   generating load.

4. **The transport decision on record is not the shipped product.** ADR-0007,
   `docs/protocol.md:3` and `docs/architecture.md:24` all state TCP framed is the
   primary transport per ADR-0007. The desktop has no TCP or UDP client at all — only
   `MjpegClient` over HTTP port 81 — and the firmware's raw transport is compiled out
   by default (`Kconfig.projbuild:5` `default n`). The release README says
   "MJPEG over HTTP". `docs/architecture.md:113` quietly states the opposite of its
   own line 24. Two accepted documents and the code disagree; the code plus the
   release artifact are authoritative.

5. **The recovery path silently reverts every sensor control the user set.**
   `camera_driver_init()` resets brightness, contrast, saturation, white balance, AWB,
   exposure, gain, h-mirror and v-flip to hard-coded defaults (`camera.c:226-234`) on
   *every* init. `camera_control_init()` — which restores them from NVS — is called
   exactly once, at boot (`app_main.c:113`). So any watchdog reboot, any
   `camera_recover()` after an overflow, or any resolution/quality change resets the
   user's image tuning to zero, permanently, and the UI still shows the old values
   until the next status poll corrects them.

**Phase status.** Phases 0–5 are complete with reproducible evidence. Phase 6 is
*implemented but insufficiently validated* — the architecture is coherent and largely
correct, but three of its ten subsystems (recording, reconnect, profile data) have no
automated test covering their actual risk. Phase 7 is substantially started (theme
layer, light/dark, data-driven sensor controls) but not complete: §28 hardware
capability detection is entirely absent, and the packaging/licensing/distribution work
that Phase 7 implies is unfinished. Phase 8 has not started; the ≥1 h soak gate is open
and correctly labelled as such everywhere.

**Headline risk.** Not correctness of the live stream — that is measured, bounded and
honest. The risk is that a *release-quality* product ships on top of a test suite that
a clean checkout never runs, and that two of the three fixes below (1 and 2) are
one-line changes nobody has made because the tests that would have caught #2 cannot
execute on a fresh clone.

---

## 2. Canonical Specification / Source-of-Truth Interpretation

### 2.1 The two Master Prompt files

`Master Prompt — ESP32-CAM to Qt Professional Low-Latency Video System.md` (em-dash,
1,603 lines) and `Master Prompt - ESP32-CAM to Qt Professional Low-Latency Video System.md`
(hyphen, 1,603 lines) are **byte-identical after line-ending normalisation**:

```
$ diff <(tr -d '\r' < "...-  .md") <(tr -d '\r' < "...—.md") | wc -l
0
```

They differ only as CRLF (hyphen) vs LF (em-dash). Per the audit brief, the em-dash file
is treated as canonical. **However**, the em-dash file is the *stale* one: `git log`
shows it was last touched at `fdab851` (repository foundation) while the hyphen variant
was modified at `f3258d5` (the current HEAD). The content is currently identical, so
this is a latent hazard rather than a live conflict — but the canonical designation and
the most-recently-edited designation point at different files. See DD-13.

### 2.2 The network topology conflict

This is the conflict the brief asked me to resolve carefully.

- **Master Prompt §5** (lines 163-189) specifies ESP32-CAM and PC both join a shared
  Wi-Fi **router**: "The ESP32-CAM and PC/HMI connect to the same Wi-Fi router."
- **ADR-0006** (accepted 2026-09-23, user-directed) replaces this with **softAP only**:
  ESP32 broadcasts `ESP32-CAM`, PC joins it directly, camera at fixed `192.168.4.1`.
  It states explicitly: "This **deviates from Master Prompt §5** (both devices on the
  same factory router) — deviation approved by the user on 2026-09-23. Supersedes
  ADR-0001."
- **AGENTS.md** "Locked architectural decisions" mandates softAP (ADR-0006).
- **Source confirms softAP.** `wifi.c:38-45` sets `WIFI_MODE_AP` only, channel 1,
  `max_connection 4`, `WIFI_AUTH_WPA2_PSK`, `WIFI_PS_NONE`. There is **no station-mode
  code anywhere in the repository** — I grepped for it. `app.h:19` pins `AP_IP
  "192.168.4.1"`.
- **Every benchmark was measured on the direct softAP link**, recorded as RSSI per run
  in each raw artifact.

**Authoritative position: softAP (ADR-0006) governs.** Under the project's stated
source-of-truth order, an accepted ADR and AGENTS.md outrank the Master Prompt as a
*specification of intended behaviour*; the Master Prompt is the product requirement,
and this requirement was consciously superseded by the project owner with a recorded
rationale. No benchmark data was invalidated, because the topology was switched before
the Phase 3 measurement campaign.

**Recommended action:** amend Master Prompt §5 to state the softAP topology as the
adopted baseline with a pointer to ADR-0006, rather than leaving the two documents in
open contradiction. This is documentation work, not code work, and it should be done
because the next agent to read the Master Prompt alone will build the wrong thing.
Multi-camera scale-out is genuinely affected — one network per camera — and that
consequence is recorded in ADR-0006 but not in the Master Prompt.

### 2.3 The source-of-truth order I applied

1. Actual hardware behaviour
2. Official vendor / SDK documentation
3. `AGENTS.md` project rules
4. Accepted ADRs and recorded decisions
5. Actual source code
6. Reproducible automated/manual tests
7. Reproducible benchmark artifacts
8. Master Prompt as intended product specification
9. Assumptions

Applied consistently, this order is what resolves the transport contradiction
(§17, DD-1) in favour of the code and the release artifact, and the sensor-reset
finding in favour of source over ADR-0009's silence on the subject.

---

## 3. Current Git / Repository State

| Field | Value |
|---|---|
| Branch | `master`, tracking `origin/master`, in sync |
| HEAD | `f3258d5` "feat(desktop): a theme layer, so dark and light are one property" |
| Working tree | **clean** — `git status --porcelain` empty, no staged, no untracked |
| Commits | 50 total, tags `v0.1.0` and `v0.2.0` |
| Tracked files | 161 |
| Build state | `desktop/build` is **current**: `ninja -n` reports "no work to do" |
| Tests | 12/12 ctest suites pass (16.36 s), run by me during this audit |
| Toolchain | Qt 6.11.2 mingw_64, CMake 3.30.5, Ninja 1.12.1, MinGW 13.1.0, ESP-IDF v5.5.4 |

**Phase-6 and Phase-7 commit history (most recent first):**

| Commit | Nature |
|---|---|
| `f3258d5` | Theme layer — Phase 7 visual architecture |
| `62ec6ed` | Live-device byte-identity capture test |
| `168fbd3` | HD/q12 scene-dependence benchmark documentation |
| `9beced1` | **Five defects found by driving the built app** — settings panel binding, notification removal, profile selector stringification, `setConfigQuery` single-slot drop, double-polling placeholder |
| `427f98c` | `v0.2.0` — section-26 audit gaps |
| `4b5dac2` | Phase 6 architecture + test inventory docs + 0.2.0 |
| `aca9151` | Diagnostics + notifications (ADR-0014) |
| `6c2db2e` | Measured operating profiles (ADR-0012) |
| `44e1093` | Per-camera device objects behind a registry (ADR-0013) |

`9beced1` is significant context: the previous agent ran the built application under UI
automation, found five real defects, and fixed each with a test where one was possible.
That is the right process, and it is also evidence that **static review of this codebase
does not find everything** — the defects it found were behavioural, not structural.

**Ignored-but-present:** `desktop/build/`, `firmware/esp32_cam_stream/build/`,
`firmware/esp32_cam_stream/sdkconfig`, `dist/`, `audits/`, `session-0.md`,
`firmware/.../main/config_secrets.h` (correctly ignored), `managed_components/`.

---

## 4. Architecture Reconstruction

### 4.1 Firmware (ESP-IDF v5.5.4, esp32, esp32-camera 2.1.7)

Verified against `sdkconfig.defaults`, `Kconfig.projbuild`, `app.h` and source:

| Concern | Actual state | Authority check |
|---|---|---|
| Topology | softAP `ESP32-CAM`, ch 1, WPA2-PSK, max 4 clients, fixed `192.168.4.1` | ADR-0006 **satisfied exactly** |
| Camera init | HD, q12, XCLK 18 MHz, fb3, `CAMERA_GRAB_LATEST`, PSRAM, `PIXFORMAT_JPEG`, `fb_size=0` (correctly ignored under `..._CUSTOM`) | ADR-0010 **satisfied** |
| Frame budget | `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM=y`, `=262144` | ADR-0009 **satisfied** |
| Socket budget | `CONFIG_LWIP_MAX_SOCKETS=16`, `TCP_FIN_WAIT_TIMEOUT=5000`, control httpd `max_open_sockets=6` + `recv_wait_timeout=2`, stream httpd `max_open_sockets=2` | architecture.md's 7+5=12 (control is 6+3=9 now, not 7 — see DD-6) |
| Raw transport | `CONFIG_SORTING_CAM_FRAME_TRANSPORT` default **n** | Correct: no shipped client uses it |
| Camera mutex | Taken at `camera_fb_get`, held across the frame's lifetime, released at `camera_fb_return` | ADR-0009 design constraint **honoured** |
| Auth | PBKDF2-HMAC-SHA256, 8192 iterations, 16 B salt, 32 B verifier, 16 B nonce, 16 B token, max 4 tokens, `ct_equal` constant-time compare, 8-failure lockout with exponential backoff | Real construction, correctly implemented (FW-15 on iteration count) |
| NVS persistence | `camcfg`/`v1`, magic + version + CRC32, range-checked on restore | ADR-0009 **satisfied** except enum fields (FW-2) |

### 4.2 Desktop (Qt 6.11.2, C++17, LGPL modules only)

| Layer | Type | Thread | Verified |
|---|---|---|---|
| UI | `Main.qml` (1,772 lines) + `Theme.qml` singleton (108) | GUI, presentation only | No networking, no JSON, no file I/O, no per-frame JS |
| Discovery | `DiscoveryService` (UDP :48888) | own QThread | dedupes by device id, not address |
| Registry | `DeviceRegistry` | GUI | owns every `CameraDevice` |
| Session | `CameraDevice` (ADR-0013) | GUI, owns the rest | per-instance, no singleton |
| Control | `DeviceStatus` → `AuthClient` → `CredentialStore` | own QThread | DPAPI, Bearer + 401 |
| Transport | `MjpegClient` | net thread (worker only) | chunked multipart parser |
| Decode | `MjpegWorker` | **net thread** | feed-and-run, matches ADR-0010 rationale |
| Pipeline | `FrameBus` (image + raw bytes) | **GUI thread writes**, render reads | ⚠ contradicts architecture.md:125 |
| Render | `FrameImageProvider` | render thread | resolves the active bus per request |
| Record | `Recorder` (`.scamrec`) + `SnapshotWriter` | **⚠ GUI thread** | contradicts architecture.md:128 |
| Profiles | `ProfileEngine` (ADR-0012) | GUI | ⚠ one borrowed data point |
| Notices | `NotificationCenter` | GUI | shared severity vocabulary |
| Prefs | `UserPreferences` (QSettings) | GUI | separate from `CredentialStore` |

**Two documentation contradictions fall out of the thread model** (DD-5, DD-7):
`architecture.md` says the net thread writes the bus and that recording runs on the net
thread. Both are false — `MjpegClient` is never moved to its own thread, so anything
connected to `frameReady` executes on the GUI thread. The decode claim is correct; the
bus-write and record claims are not.

---

## 5. Phase 0–6 Audit

| Phase | Intended scope | Actually implemented | Evidence | Verified | Remaining | Risk |
|---|---|---|---|---|---|---|
| 0 | Repo/env discovery | Complete | `fdab851`, `AGENTS.md`, 4 toolchain scripts | Yes | None | LOW |
| 1 | Architecture analysis | Complete | 10 ADRs, `architecture.md`, `protocol.md` | Yes | §5 topology text still stale in Master Prompt | LOW |
| 2 | Minimal baseline | Complete | `7e94b4c`, `6444e6d`; live-verified per ADR-0006 checklist | Yes | None | LOW |
| 3 | Transport benchmark | Complete, **conclusion not implemented** | 6 raw JSON + 3 aggregates, all match docs exactly | Yes | ADR-0007's decision was never built (DD-1) | MED |
| 4 | Camera matrix | Complete | `phase4-20260925-fixed.jsonl`, 74 cells; every envelope/fbgrab/confirm figure verified by me | Yes | None | LOW |
| 5 | Pipeline optimization | Complete | 20 raw JSON; XCLK sweep, fb A/B, affinity A/B, gate arithmetic | Yes | Wi-Fi/LwIP knobs dropped with reason recorded (acceptable) | LOW |
| 6 | Product architecture | **PARTIALLY COMPLETE** | 9 feature commits, 12 test suites, 5 ADRs | Partly | 3 subsystems untested; 2 data/lifecycle defects (CP-1, DX-2) | **HIGH** |
| 7 | Premium UI | **PARTIALLY COMPLETE** | `f3258d5` theme layer; 241 token refs, 0 literals | Partly | §28 absent; packaging/licensing incomplete; 1 UI defect | MED |
| 8 | Validation | **NOT STARTED** | Zero phase-8 artifacts; soak correctly deferred | Yes (as "not started") | Entire program | HIGH (gate open) |

---

## 6. Detailed Phase 6 Correctness Review

Findings ordered by severity. Every one was verified against source by me or by a
subagent whose specific claim I then re-checked independently.

### CP-1 — CONFIRMED DEFECT — P0 — Tests do not build from a clean clone

- **Files:** `desktop/CMakeLists.txt:90`, `tests/CMakeLists.txt:1`
- **Evidence:** `if(BUILD_TESTING)` at line 90 guards `add_subdirectory(../tests)`.
  `include(CTest)` — the only thing that defines `BUILD_TESTING` — appears **nowhere**
  in the repository. `desktop/build/CMakeCache.txt:18` shows
  `BUILD_TESTING:UNINITIALIZED=ON`, meaning it was typed by hand.
- **Proof (both directions, run by me):**
  - Documented configure → **0** `tst_` targets, **no** `CTestTestfile.cmake`, `tests/`
    subdirectory absent.
  - `-DBUILD_TESTING=ON` added → **60** `tst_` targets, `CTestTestfile.cmake` present.
- **Expected:** a clean checkout following `AGENTS.md` builds the 12 suites.
- **Root cause:** `enable_testing()` was called *inside* the `if(BUILD_TESTING)` block
  instead of `include(CTest)` before it.
- **Why it matters:** every quality claim in the repository — "12/12 suites green as of
  the 0.2.0 release" (`docs/testing.md:23`) — is unreproducible by a new agent. The
  previous agent's own test-driven fix for CP-2 could not have been caught by CI.
- **Confidence:** high (empirically proven both ways).

### CP-2 — CONFIRMED DEFECT — P1 — Balanced profile carries a borrowed benchmark figure

- **Files:** `desktop/src/ProfileEngine.cpp:92`
- **Evidence:** `p.medianBytes = 14680.0` for the svga/q24 "Balanced" profile. I
  searched all 74 Phase-4 cells: `frame_bytes.p50 == 14680` occurs in exactly one cell —
  **vga/q24, fb2, confirm stage**. The genuine svga/q24 measurements are:
  - envelope, fb3: **16,982 B**, 22.50 fps
  - confirm, fb2: **23,077 B**, 14.37 fps
- **Expected:** `medianBytes` reflects the profile's own configuration, or 0 with the
  evidence string saying so — the pattern the file already uses for
  `altMeasuredFps = 0.0` one line above.
- **Root cause:** the number was copied from the neighbouring rung of the confirm ladder.
- **Why it matters:** `ProfileEngine.h:46-49` states the rule in its own comment —
  "putting a VGA number on an SVGA profile would be a fabricated data point." Line 92
  does precisely that. AGENTS.md forbids publishing a benchmark number that was not
  measured on that configuration. The user-visible consequence is a frame-size estimate
  that is 13% low for Balanced.
- **Confidence:** high (exhaustive search of raw artifacts).

### CP-3 — CONFIRMED DEFECT — P1 — Recording and the frame bus run on the GUI thread

- **Files:** `desktop/src/CameraDevice.cpp:170-178`, `desktop/src/Recorder.cpp:150-208`,
  `desktop/src/MjpegClient.cpp:423-429`
- **Evidence:** `MjpegClient`'s constructor moves only `m_worker` to `m_thread`; the
  client itself is never moved. `frameReady` is emitted from a lambda on `MjpegClient`,
  so the `connect(m_stream, &MjpegClient::frameReady, this, ...)` in `CameraDevice` is a
  **queued** connection landing on the GUI thread. Therefore `FrameBus::setFrame` and
  `Recorder::appendFrame` both execute there.
- **Per-frame cost:** two `write()` calls on an `Unbuffered` `QFile`
  (`Recorder.cpp:103`, `:154-155`). **Per-60-frames cost:** `writeIndex()` serialises the
  entire `m_frames` table to a `QJsonArray` and commits a `QSaveFile` — O(N) each time,
  O(N²/60) over a recording.
- **Expected:** AGENTS.md — "GUI thread never does network I/O or JPEG decode"; the
  recorder belongs on the frame-delivery path, which the design intends to be the net
  thread (`architecture.md:128`).
- **Why it matters:** a 10-minute 45 fps recording ends with a ~27,000-entry (~2 MB) JSON
  build plus an atomic rename on the GUI thread every 1.3 s. The user is explicitly
  creating this load by recording. This is the §34 "responsive UI" acceptance criterion
  degrading exactly where a user chooses to put load on it.
- **Confidence:** high (thread-affinity traced end to end).

### CP-4 — CONFIRMED DEFECT — P1 — Camera recovery silently reverts all sensor controls

- **Files:** `firmware/esp32_cam_stream/main/camera.c:226-234`, `app_main.c:113`
- **Evidence:** every `camera_driver_init()` unconditionally sets
  `set_brightness(0)`, `set_contrast(0)`, `set_saturation(0)`, `set_whitebal(1)`,
  `set_awb_gain(1)`, `set_exposure_ctrl(1)`, `set_gain_ctrl(1)`, `set_hmirror(0)`,
  `set_vflip(0)`. `camera_control_init()` — which restores them from NVS — is called
  **exactly once**, at boot (`grep` confirms one call site).
- **Triggered by:** watchdog `esp_restart()`, `camera_recover()` after a frame-buffer
  overflow, and any framesize/fb_count/fb_location/grab_mode/xclk change (all of which
  deinit/init).
- **Expected:** the operating point *and* the image settings survive recovery — ADR-0009
  already made this argument for the operating point and stored it in NVS for exactly
  this reason. Sensor controls were simply left out of the same treatment.
- **Why it matters:** a user who tunes brightness and contrast has them silently reset
  to zero by any camera hiccup, with no notification. Given the ~2 fps UXGA stall and
  the QVGA q4 overflow incidents in the benchmark record, this is not a theoretical path.
- **Confidence:** high.

### CP-5 — CONFIRMED DEFECT — P1 — Recovery-request counter is never reset

- **Files:** `desktop/src/MjpegClient.cpp:466-467`, `MjpegClient.h:89`
- **Evidence:** `m_recoveriesTriggered` is incremented and tested but **never assigned
  anywhere else** — not in `start()` (`:646-666`), not in `stop()` (`:668-681`), not in
  the `frameReady` handler (`:492-501`). Only `m_noResponseStreak` is reset (`:497`).
- **Expected:** a self-healing mechanism that re-arms after the camera has been healthy.
- **Why it matters:** after two camera-recovery requests anywhere in the process
  lifetime, `deviceRecoveryRequested` can never fire again. A camera that runs cleanly
  for an hour and then wedges gets no self-repair. This is the documented
  "no response from camera" recovery path in `docs/protocol.md:103`.
- **Confidence:** high (exhaustive grep — 3 occurrences, no reset).

### CP-6 — CONFIRMED DEFECT — P1 — A 401 on a config write silently drops the user's change

- **Files:** `desktop/src/DeviceStatus.cpp:249-252`, `:924`
- **Evidence:** `m_pendingKv.clear()` runs at `:924` *before* the request is sent. On a
  401 reply the handler calls `m_auth->signIn()` and emits
  `configFinished(false, "session expired, signing in again")` — wording that implies a
  retry. No retry is queued. `m_cfgRetries` guards only the 409 path.
- **Why it matters:** a token that expires during normal operation (or a camera that
  restarts and loses its token table) causes every settings change in that window to be
  discarded while the UI shows a transient message. The user's brightness change simply
  does not happen. Same path exists for `setSensor`.
- **Confidence:** high.

### CP-7 — CONFIRMED DEFECT — P1 — Session-reconnect watchdogs never tested; §33 gap

- **Files:** `tests/` (absence), vs `MjpegClient.cpp:12-19`, `docs/protocol.md:103`,
  `docs/architecture.md:228-230`
- **Evidence:** the ladder is `kMaxRetries=5`, delays `{500,1000,2000,3000,5000}`,
  reset only on a decoded frame, 6 s first-byte watchdog, 8 s stall watchdog. I verified
  the *implementation* is correct. **No test anywhere exercises it.** The `MjpegStub` in
  `tst_capture` never fails, never stalls, never closes.
- **Why it matters:** this is the mechanism that keeps a dead camera from masquerading
  as a live stream — the §24 "no reconnect storms" requirement. A regression that reset
  the counter on TCP connect would ship green.
- **Confidence:** high (absence verified across all 12 suites).

### CP-8 — CONFIRMED DEFECT — P2 — The tested wire format is not the shipped wire format

- **Files:** `tests/tst_capture.cpp:59-88` vs `firmware/.../http_servers.c:364-369`
- **Evidence:** the test stub emits `Content-Type: multipart/x-mixed-replace; boundary=--FRAME`
  with per-part `Content-Length` and **no** `Transfer-Encoding`, so the parser runs its
  `HttpState::Identity` branch. The firmware uses `httpd_resp_send_chunk` → chunked.
  The `ChunkSize`/`ChunkData`/`ChunkEnd` states are never executed by any test.
- **Why it matters:** a bug in the chunk decoder — the code path the real device uses on
  every frame — is invisible to the entire suite. `tools/fake_camera.py:447` *does* send
  chunked, and is started by nothing.
- **Confidence:** high.

### CP-9 — CONFIRMED DEFECT — P2 — No malformed-input tests; six `fail()` branches are dead to CI

- **Evidence:** no test drives garbage `Content-Length`, a part without `\r\n\r\n`, a
  bad chunk size, a non-numeric boundary, or a >8 MiB part. The bounds *are* present in
  code (`kMaxBufferBytes` 8 MB at `MjpegClient.cpp:13`, checked at `:206`; headers 8 KB,
  chunk line 128 B, part headers 2 KB) and I confirmed they hold — so the PC cannot be
  OOM'd by a bad camera. But the invariant has no regression guard.
- **Why it matters:** `docs/architecture.md` and AGENTS.md both assert bounded buffers.
  That claim is currently true and untested; any future edit could break it silently.
- **Confidence:** high. I verified the bounds are real, so this is a test gap, not a
  live defect.

### CP-10 — CONFIRMED DEFECT — P2 — Three status overlays occupy identical coordinates

- **Files:** `desktop/qml/Main.qml:1737`, `:1750`, `:1763`
- **Evidence:** all three are `Label`s with `anchors.left: parent.left`,
  `anchors.bottom: parent.bottom`, `anchors.margins: 14`, and no `anchors.top`/offset.
  They are: the reconnect counter (`visible: sessionState.state === "reconnecting"`), the
  camera-recovery counter (`visible: … camera_recoveries > 0`), and the stream error
  (`visible: stream.errorString.length > 0`).
- **Why it matters:** the conditions are independent. After any camera recovery
  (`camera_recoveries` is cumulative for the boot), a later stream failure shows the
  error text *underneath* the recovery counter at the same 14 px margin. The user sees
  one of the two, and which one is z-order dependent.
- **Confidence:** high. Confirmed by inspecting anchor blocks; conditions are provably
  co-satisfiable.

### CP-11 — CONFIRMED DEFECT — P2 — `releaseStale()` has no production caller

- **Files:** `desktop/src/DeviceRegistry.cpp:100-123`, `main.cpp:135-137`
- **Evidence:** `grep` finds callers only in `tests/tst_deviceregistry.cpp`. Every
  `acquire()` with a new `deviceId` constructs a full `CameraDevice`: an `MjpegClient`
  QThread, a `DeviceStatus` QThread, a 1 s profile ticker and a `NotificationCenter` with
  a 500 ms sweep timer.
- **Why it matters:** a device announcing with rotating `device_id`s spawns thread pairs
  without bound. Single-camera today, but §27 asks that multi-camera not be made
  unnecessarily difficult, and this is the seam. The correct behaviours — active device
  never collected, unknown `setActive` is a no-op — are implemented and tested.
- **Confidence:** high.

### CP-12 — CONFIRMED DEFECT — P2 — `applyNextConfig` abandons later options on a gap

- **Files:** `desktop/main.cpp:246-255`, `:269-273`, `:278-282`
- **Evidence:** the `switch` on `configSlot` returns `false` without advancing
  `configSlot` when `parser.isSet(...)` is false. Every caller reads `false` as
  "nothing left to apply" and calls `scheduleStream()`.
- **Why it matters:** `--bench 20 --quality 12` without `--framesize` never applies the
  quality — a benchmark that silently measures the wrong configuration. The chained
  `configBusyChanged` gate on `configSlot < configCount` then never clears.
- **Confidence:** high (control-flow traced).

### CP-13 — CONFIRMED DEFECT — P2 — `per-read` recomputation costs ~600 wasted allocations/second

- **Files:** `desktop/src/SessionState.cpp:127-128`, `MjpegClient.cpp:216`,
  `CameraDevice.cpp:111-121`
- **Evidence:** `statsUpdated` is emitted on **every** `readyRead`. `SessionState::observe`
  hooks it and runs `recompute()`: 15 cross-object property reads plus a full `derive()`
  building 2-4 `QString::arg()` detail strings — all discarded by the `out == m_outcome`
  early-out. At 45 fps with multiple reads per frame that is 100-200 Hz.
- **Why it matters:** measurable GUI-thread waste for a value that changes at most a few
  times per second. Not a stall, but it is on the same path as CP-3.
- **Confidence:** high.

### CP-14 — CONFIRMED DEFECT — P2 — Sensor sliders lose their status binding on first drag

- **Files:** `desktop/qml/Main.qml:1502` vs `:1223-1227`
- **Evidence:** sensor sliders use `value: parent.current`. A user drag assigns `value`
  imperatively, which **destroys the binding** — after one drag the slider stops tracking
  device status permanently. The quality slider does this correctly via an explicit
  `syncFromStatus()` guarded by `!pressed`.
- **Why it matters:** the 1 Hz status poll can also yank a slider out from under a
  user's finger, and after one drag the UI shows a value the camera does not have.
- **Confidence:** high.

### CP-15 — CONFIRMED DEFECT — P2 — Notifications dismissed by index, which `postOnce` invalidates

- **Files:** `desktop/qml/Main.qml:658`, `NotificationCenter.cpp:108-124`, `:199-212`
- **Evidence:** `notify.dismiss(notifyCard.index)`. `postOnce` performs
  `beginRemoveRows` + `beginInsertRows(0,0)`, shifting every delegate index;
  `sweepExpired` can also remove a row every 500 ms.
- **Why it matters:** this is the exact defect `9beced1` fixed on the C++ side
  (dismissal was changed to retract by condition via `dismissKey()`), reintroduced in
  QML. `data()` exposes no key role, so QML *cannot* use the safe API. Between the click
  and the handler the index may address a different card.
- **Confidence:** high.

### CP-16 — CONFIRMED DEFECT — P2 — HighQuality profile held to an invented floor

- **Files:** `desktop/src/ProfileEngine.cpp:51-73` vs `docs/benchmark-results.md:447`
- **Evidence:** ADR-0010 scoped HD to a provisional ≥7 fps. But
  `benchmark-results.md:447-457` is headed "**HD/q12 is an experimental profile, not the
  production default**" and reports 7.7-8.4 fps, stating plainly that "The production
  default is not yet chosen." The ladder ships HD q12 as the production default with
  `floorFps = 7.0` — a floor chosen so the profile passes its own test.
- **Why it matters:** the *decision* (HD is the default) is the project owner's and is
  correctly recorded in ADR-0010. What is wrong is the presentation: a provisional,
  below-target floor presented as settled, and a comment claiming a default status that
  the benchmark document explicitly withholds. This is the same honesty failure ADR-0012
  was written to prevent for UXGA.
- **Confidence:** high on the contradiction; the owner decision itself is legitimate.

### CP-17 — CONFIRMED DEFECT — P2 — Three mutually exclusive claims merged into one evidence string

- **Files:** `desktop/src/ProfileEngine.cpp:68`, `:73`
- **Evidence:** `altMeasuredFps = 9.98; // mean of the three D2 runs 10.00/9.97/9.96`.
  I opened `phase6-baseline-20260928-hd-q12-x18.json`: it is **one** 120 s run. The three
  numbers are `device_summary.capture_fps` (10.0), `delivery_fps` (9.966) and
  `app.decoded_fps` (9.958) — three *measures of one run*, whose agreement is the
  evidence that the PC keeps up, not a repeatability measurement. The evidence string
  also says "58 KB" while the artifact's own mean frame size is **28,956 B**; 58 KB
  belongs to the 2026-09-29 recheck runs.
- **Why it matters:** ADR-0012 lists this artifact as "the only ladder point with a
  three-run measurement." It is not three runs. The claim that matters — repeatability —
  is unevidenced, and HD is the one profile whose floor is already provisional.
- **Confidence:** high (artifact opened and the three fields located by name).

### CP-18 — CONFIRMED DEFECT — P2 — NVS restore skips enum validation

- **Files:** `firmware/esp32_cam_stream/main/camera.c:331-342`
- **Evidence:** `framesize`, `quality`, `fb_count` and `xclk_mhz` are all range-checked
  on restore. `rec.grab_mode` and `rec.fb_location` are cast straight to their enums
  with no check. The *API* path does validate both (`camera.c:444-445`).
- **Why it matters:** low practical risk (CRC + magic + version gate it, and only this
  firmware writes the blob), but it is an inconsistency in a function whose entire job
  is validating untrusted restored state. A future schema migration or a hand-edited
  NVS partition passes an arbitrary byte into `esp_camera_config_t`.
- **Confidence:** high on the asymmetry; low on exploitability.

### CP-19 — CONFIRMED DEFECT — P2 — Camera mutex held across network sends

- **Files:** `firmware/.../http_servers.c:364-370`, `camera.c:403-424`
- **Evidence:** `camera_fb_get` takes `s_cam_mutex`; the handler holds it across three
  `httpd_resp_send_chunk` calls before `camera_fb_return`. `send_wait_timeout` is **not**
  set (the httpd config at `:697-702` sets only `max_open_sockets` and `stack_size`), so
  ESP-IDF's 5 s default applies. Same shape in `snapshot_handler` and in
  `frame_transport.c` (`TX_TIMEOUT_S 5`).
- **Why it matters:** a slow or paused TCP client stalls every other camera consumer for
  up to 5 s per frame and blocks the watchdog's `camera_recover()` on
  `portMAX_DELAY`. A client reading at a trickle can trigger a spurious power-cycle
  recovery. Network I/O should not sit inside the camera lock.
- **Confidence:** high on the mechanism; the default 5 s value follows ESP-IDF's
  `httpd_config_t` documentation.

### CP-20 — CONFIRMED DEFECT — P3 — `app_version` is not reported by the device

- **Files:** `docs/protocol.md:70` vs `firmware/.../http_servers.c:56-58`,
  `discovery.c:58-59`
- **Evidence:** the doc claims "Firmware version, protocol version, **app version** each
  reported in status/discovery." Status and capabilities report `fw_version` and
  `proto_version` only. `app_version` exists solely as a client→recorder field
  (`Main.qml:139`). No firmware code path reads it.
- **Why it matters:** §39/§13 provenance claims cannot be satisfied by a device that
  never learns the app version. The recording header already carries it, which is the
  right place — the doc sentence is the thing that is wrong.
- **Confidence:** high.

### CP-21 — CONFIRMED DEFECT — P2 — No protocol-compatibility check on the desktop

- **Files:** `desktop/src/DiscoveryService.cpp:228`, `docs/protocol.md:69`, §39
- **Evidence:** `proto_version` is parsed and surfaced to QML. There is no
  `kProtoVersion` constant in `desktop/` and no mismatch comparison anywhere
  (`grep mismatch|incompatib` → zero hits in `desktop/src` and `desktop/qml`).
- **Why it matters:** §39 says "a future firmware update must not silently break an
  older desktop application." The exact failure mode is unguarded. One constant plus one
  comparison closes it.
- **Confidence:** high.

### CP-22 — CONFIRMED DEFECT — P2 — `CredentialStore` test writes to the real user registry

- **Files:** `tests/tst_credentialstore.cpp:20-24`
- **Evidence:** the suite sets org/app to `SortingCameraTest` but never calls
  `QSettings::setPath`, so DPAPI-protected blobs are written to and deleted from the
  real `HKCU` registry. `tst_userprefs` does this correctly with a `QTemporaryDir`.
- **Why it matters:** a failing or aborted run leaves entries behind. The org name
  differs from the app's so it cannot clobber a real stored password, but a test should
  not touch global state.
- **Confidence:** high.

### CP-23 — CONFIRMED DEFECT — P2 — Footer "drop" metric conflates decode failures with drops

- **Files:** `desktop/qml/Main.qml:803-811`, `MjpegClient.cpp:393-396`
- **Evidence:** the label reads `stream.framesDropped + frameBus.overwrittenCount` with a
  comment describing "frames the transport discarded." `mjpeg.framesDropped` is
  incremented **only on decode failure**, not on any transport-level discard.
- **Why it matters:** AGENTS.md requires every drop to be counted and never hidden. The
  visible metric conflates two different things and understates both.
- **Confidence:** high.

### CP-24 — CONFIRMED DEFECT — P3 — Firmware version string reaches the wire via ESP-IDF default

- **Files:** `firmware/.../http_servers.c:56`, `firmware/esp32_cam_stream/CMakeLists.txt:12`
- **Evidence:** the handler prefers `esp_app_get_description()->version`, which the build
  stamped as `v0.1.0-19-g427f98c` (confirmed in `project_description.json` and by
  `strings` on the shipped `.bin`). `FW_VERSION "0.2.0"` (`app.h:9`) is only the
  `app == NULL` fallback, and no `project(... VERSION ...)` line exists in the firmware
  CMakeLists to make them agree.
- **Why it matters:** the operator-visible "fw" field in the UI disagrees with the
  release version, and the version an operator reports from the field is therefore
  ambiguous. This is also why every `phase6-*.json` artifact records
  `fw_version: v0.1.0-7-…` for a 0.2.0-branded release.
- **Confidence:** high.

### CP-25 — CONFIRMED DEFECT — P3 — ProfileEngine's documented "conservative reading" rule is not implemented

- **Files:** `desktop/src/ProfileEngine.cpp:174-192`
- **Evidence:** the comment promises taking the more conservative of the two
  measurements when both exist; the code tests only `p.measuredFps >= p.floorFps` and
  never consults `altMeasuredFps`. For all four measured profiles `alt >= measured`, so
  the answer coincides today.
- **Why it matters:** a latent trap. The moment a profile's confirm run comes in *below*
  its envelope run, the UI will report the optimistic number.
- **Confidence:** high that the rule is absent; no current behavioural difference.

---

## 7. Phase 7 Current Status

**Phase 7 is genuinely started, not untouched.** `f3258d5` landed a real theme
architecture, and it is well executed.

### 7.1 Complete and verified

| §25/§26 requirement | State | Evidence |
|---|---|---|
| Theme architecture | Complete | `Theme.qml` 108 lines, `pragma Singleton`, 241 `Theme.` refs in Main.qml, **0** hard-coded hex colours, 21 `font.family` → token |
| Dark/light | Complete | `Theme.mode` property; `QQuickStyle::setStyle("Basic")` + `QPalette` role mapping (roles only — naming a group like `disabledText` would fail the load) |
| Touch readiness | Complete | `Theme.touchTarget = 44`, applied at 25 sites; `implicitWidth/Height` at 27 sites |
| QML/C++ separation | Complete | Zero `XMLHttpRequest`, `JSON.parse`, `Qt.createComponent`, or file I/O in Main.qml; no per-frame JS beyond one URL concat |
| Data-driven sensor controls | Complete | Controls render from `deviceStatus.capabilities`, not a hard-coded list — satisfies §20's "do not expose a control merely because a generic UI has it" |
| Fullscreen | Complete | F11 shortcut + `Esc` to exit |
| Zoom | Complete | Scene-graph `ScaleTransform` (`:967-970`) — display-only, no re-encode, per §23 |
| Diagnostics view | Complete | F12 on-demand toggle, rate-limited, severity-coloured |
| Notifications | Complete | Info expires 8 s, warning+ sticky, per-key dedup (ADR-0014) |
| Keyboard shortcuts | Complete | 7 `Shortcut` elements: F11, F12, Esc, Ctrl+0, Ctrl+T, Ctrl+Plus, Ctrl+Minus |
| Error states | Complete | Session-state overlay, connection status text, config error, sensor error |
| Settings | Complete | Three capture settings + capture directory, through `UserPreferences` |

### 7.2 Incomplete

| Item | § | Severity | Note |
|---|---|---|---|
| **Hardware capability detection** | §28 | **P1** | **Entirely absent.** No `QSysInfo`, no `QScreen` geometry/DPI, no RHI/graphics-backend query, no touch-capability probe, no decode-acceleration check. AGENTS.md forbids hard-coded Intel UHD assumptions; the app currently makes no assumptions *and performs no detection*, so a future HMI with different characteristics gets no adaptation and no diagnostic. `docs/deployment.md:12` promises this at startup. |
| **Accessibility** | §25 | P2 | Zero `Accessible.*` properties in 1,772 lines of QML. "strong accessibility/readability" is a stated §25 characteristic; only the readability half is attempted (monospace for counters, elision, 44 px targets). |
| **Installer** | §40 | P2 | Artifact is two zips. No Qt IFW, MSIX, Inno or NSIS anywhere. `docs/deployment.md:11` still defers. |
| **Licence compliance in the package** | §29 | P1 | The shipped zip contains **no LICENSE, NOTICE, COPYING or attribution of any kind** (0 of 1,388 entries). Dynamic linking is genuinely correct, so the LGPLv3 *mechanism* holds; the §4 notice/relinking obligations do not. **This zip is not redistributable as-is.** |
| **Licence report currency** | §29 | P2 | All four pre-architecture-lock checkboxes are still unchecked. `QuickControls2` is linked (`CMakeLists.txt:74`) but absent from the module list. `Multimedia` is listed "under evaluation" and is not linked. ~18 shipped Qt modules unrecorded. |
| **CI** | — | P2 | No `.github/`, no pipeline config, no test runner script. Nothing ever builds or runs `tests/` automatically. |
| **Root README** | — | P2 | No `README.md`. The only user-facing text is generated into the zip. A new agent or operator has no entry point. |
| **Deployment procedure** | §40 | P2 | `docs/deployment.md:3` still reads "outline; finalize after Phase 7 packaging" for a shipped release; `:19` promises `build-firmware.ps1` + serial helper that do not exist. `package-release.ps1` is referenced **nowhere** in `docs/`. |

### 7.3 Needs correction

- **CP-10** (overlapping status overlays) — the one genuine UI defect I found.
- **CP-14** (sensor slider binding loss), **CP-15** (index-based dismissal) — QML correctness.
- Cosmetic: `Main.qml:812-814` has broken indentation on the bitrate `Label`, left by the
  theme refactor. Harmless, but it is the kind of artifact that signals an unverified
  manual pass.

### 7.4 Not required / out of scope

- Second simultaneous camera **view** — §27 explicitly permits one active stream; the
  objects behind it are per-camera (ADR-0013) and tested.
- mDNS discovery — deliberately replaced by UDP broadcast for the softAP link
  (`architecture.md:175-182`); the hybrid remains a candidate only if a station-mode
  profile ever needs to cross subnets. Correct call, properly recorded.
- TLS on the video plane — §19 explicitly excludes it.

---

## 8. Phase 7 Remaining Work and Implementation Plan

Ordered so architecture corrections precede polish that depends on them. Each step is
small and independently verifiable. Steps 1–3 are prerequisites for everything else.

### Step 1 — Make the test suite build from a clean clone
- **Objective:** eliminate CP-1.
- **Current state:** `if(BUILD_TESTING)` guards a variable CMake never defines.
- **Files:** `desktop/CMakeLists.txt:88-93`.
- **Dependencies:** none. **Effort:** one line.
- **Implementation:** add `include(CTest)` before the `if(BUILD_TESTING)` block; move
  `enable_testing()` out of the conditional. Alternatively drop the guard entirely —
  `tests/CMakeLists.txt` is cheap and the project has no reason to ship without them.
- **Tests:** configure a throwaway build dir with the exact command in `AGENTS.md`;
  assert `tst_` targets exist and `ctest -N` reports 12.
- **Manual verification:** none needed.
- **Exit criteria:** documented command → 12/12 pass on a fresh clone.

### Step 2 — Correct the Balanced profile's data provenance
- **Objective:** eliminate CP-2, satisfy AGENTS.md's "never invent benchmark numbers".
- **Files:** `desktop/src/ProfileEngine.cpp:92`.
- **Implementation:** set `medianBytes` to the measured envelope figure for the
  profile's own configuration (16,982 B, fb3 — matching `measuredFps = 22.50`'s source),
  or 0 with the evidence string extended to say the byte figure is likewise unmeasured
  for this point. The 14,680 value must not survive in any form.
- **Tests:** extend `tst_profileengine` to assert that any non-zero `medianBytes` is
  consistent with the cited evidence — at minimum, add a comment-adjacent assertion that
  no two profiles share a `medianBytes` value across different resolutions.
- **Exit criteria:** every ladder figure traces to a cell in a raw artifact.

### Step 3 — Move recording and the frame bus off the GUI thread
- **Objective:** eliminate CP-3, restore the documented thread model.
- **Files:** `desktop/src/CameraDevice.cpp:170-178`, `Recorder.*`, `MjpegClient.*`.
- **Dependencies:** Step 1 (needs a test to land against).
- **Implementation:** connect `frameReady` to a small object living on the client's net
  thread, or give `Recorder` its own worker thread with a bounded latest-frame queue.
  Preserve the guarantee that `FrameBus` and `Recorder` observe the **same** frame —
  pass both in one signal, which the current signature already does. Separately, make
  `writeIndex()` incremental (append-only sidecar) or move it off the write path.
- **Tests:** assert `Recorder` writes from a non-GUI thread (compare
  `QThread::currentThread()` inside a test hook); assert the sidecar still flushes at
  60 frames and recovers a truncated tail (existing `tst_recorder` coverage).
- **Performance:** this is a GUI-responsiveness fix; verify with the existing
  `--bench` mode that `frames_presented` stays at device fps while recording.
- **Exit criteria:** a 10-minute recording at 45 fps shows no GUI stall; measured and
  recorded.

### Step 4 — Make camera recovery preserve sensor controls
- **Objective:** eliminate CP-4.
- **Files:** `firmware/.../camera.c:226-234`, `camera_control.c`, `app_main.c:113`.
- **Implementation:** expose a `camera_control_restore()` that re-applies the NVS-stored
  sensor values, and call it after every successful `camera_try_init()` — i.e. from
  inside `camera_init()`/`camera_recover()` rather than once at boot. Cleaner still:
  move the default-setting block out of `camera_driver_init()` into an explicit
  "apply defaults" function, so an init never silently overwrites user state.
- **Tests:** firmware-side this is a live-hardware check — set a non-default brightness,
  force a recovery (the QVGA q4 overflow path is the reliable trigger), confirm the
  value survives and `/api/v1/status` reports it. Add to the Phase 8 live matrix.
- **Exit criteria:** a reboot and a recovery both preserve brightness/contrast/etc.

### Step 5 — Reset the recovery counter; re-queue dropped config writes
- **Objective:** eliminate CP-5 and CP-6.
- **Files:** `MjpegClient.cpp:466-497`, `DeviceStatus.cpp:249-252`, `:924`.
- **Implementation:** reset `m_recoveriesTriggered` wherever `m_noResponseStreak` is
  reset (on a decoded frame) and in `start()`. On a 401, restore `m_pendingKv` (or hold
  the request) and re-send after `signIn()` completes, rather than emitting a message
  implying a retry that never happens.
- **Tests:** `tst_devicestatus` — stub returns 401 once, then 200; assert the pending
  write is re-sent exactly once and the new value reaches the camera. `tst_capture` — no
  direct unit seam exists for the client counter; cover it in the Phase 8 live matrix.

### Step 6 — Close the test gaps where the risk actually is
- **Objective:** eliminate CP-7, CP-8, CP-9.
- **Files:** `tests/tst_capture.cpp`, new `tests/CMakeLists.txt` entries if needed.
- **Implementation:** (a) add a `chunked` mode to `MjpegStub` and make the primary test
  drive it, matching the firmware; (b) add a reconnect test where the stub closes the
  socket and asserts the 5-step ladder, the exact delays, and that a TCP connect alone
  does not reset the counter; (c) add malformed-input cases (bad chunk size, missing
  terminator, >8 MiB part) asserting `fail()` and the overflow guard.
- **Note:** `tools/fake_camera.py` already emulates chunked streaming, the full auth
  scheme and eight fault injections — and is started by nothing (see §14, INV-1). It is
  the obvious candidate for the integration-level half of this step.
- **Exit criteria:** the chunked decoder and the reconnect ladder both have green tests.

### Step 7 — Fix the QML correctness defects
- **Objective:** eliminate CP-10, CP-14, CP-15, CP-23.
- **Files:** `desktop/qml/Main.qml:1737/1750/1763`, `:1502`, `:658`, `:803-811`;
  `NotificationCenter.cpp:20-40` (add a key role to `data()`).
- **Implementation:** stack the three overlays with increasing `anchors.bottomMargin`;
  give sensor sliders the same `syncFromStatus()`/`!pressed` guard the quality slider
  already uses; switch dismissal to `dismissKey()` and expose the key role so QML can
  use it; correct the drop metric or rename it to what it measures.
- **Tests:** a QML-side test is not currently possible (`FrameImageProvider.cpp` is in
  the executable target, not `scamcore` — see DX-12). Manual UI-automation pass, as
  `9beced1` did.
- **Exit criteria:** no overlap in any state combination; slider tracks status after a
  drag; dismissing a card removes the card the user clicked.

### Step 8 — Add §28 hardware capability detection
- **Objective:** close the largest §-requirement gap.
- **Files:** new `desktop/src/Capabilities.*`, wired in `main.cpp`, surfaced in the
  existing F12 diagnostics panel (no new UI surface needed).
- **Implementation:** report CPU model/core count, total/available RAM, graphics backend
  and RHI API, `QScreen` geometry and logical DPI, and touch availability. No behaviour
  changes in this step — detection and display only. A later step may branch on it.
- **Tests:** assert the probe returns sane values on the dev machine; no logic to test
  until something branches on it.
- **Exit criteria:** the diagnostics panel answers every §28 bullet, and
  `docs/deployment.md:12` becomes true.

### Step 9 — Productize packaging, licensing and distribution
- **Objective:** close §29/§40 gaps and CP-1-class risk in the release path.
- **Implementation:**
  1. `package-release.ps1` must **build and test** before packaging — currently it only
     checks the exe exists and is newer than `main.cpp` (`grep ctest scripts/` → 0 hits).
     Firmware gets a rebuild or at least a freshness check; it currently only checks that
     three `.bin` files exist.
  2. Ship a `LICENSE`/`NOTICE` in the app package. Without it the zip is not
     redistributable.
  3. Fix the version split: `desktop/CMakeLists.txt:3` says 0.1.0, `main.cpp:48` says
     0.2.0, the exe manifest says 0.1.0.0, and the package name is scraped from the
     *firmware's* `FW_VERSION` — force-coupling two independent versions. One source of
     truth, threaded to all four.
  4. Record the real dependency set in `docs/licensing.md` (`QuickControls2` and ~18
     other shipped Qt modules are missing; `Multimedia` is listed but not linked) and
     close the four checkboxes.
  5. Record the **commit each artifact was built from**, per artifact. Today the app zip
     records none and the firmware README records the packaging commit, not the build
     commit — so re-running the script today would stamp `f3258d5` next to a binary
     built at `427f98c`.
  6. Set the firmware `project(... VERSION ...)` so the advertised version stops
     defaulting to ESP-IDF's git describe (CP-24).
  7. Write `docs/deployment.md` to describe what actually exists, and reference
     `package-release.ps1` from it.
- **Exit criteria:** a clean clone → `.\scripts\package-release.ps1` produces a tested,
   correctly-labelled, redistributable package.

### Step 10 — Repository hygiene
- **Objective:** make the work discoverable and repeatable.
- **Implementation:** add a root `README.md` (build, test, run, package, license);
  add a minimal CI workflow (configure → build → `ctest`) — one job, no ambition; add
  `-DBUILD_TESTING=ON` to the `AGENTS.md` example or remove the need for it; collapse
  the two identical Master Prompt files to one (DD-13).
- **Exit criteria:** a new agent can build, test, run and package from the README alone.

---

## 9. Phase 8 Final Validation Strategy

Designed against the **actual** final architecture, not a replay of the Phase 5
streaming procedure. Reuse what exists: `benchmarks/phase5_pipeline.py` (app bench mode
+ device series slicing + validity report), `tools/fake_camera.py` (8 fault injections),
the 12 ctest suites, and `AppMetrics`.

### A. Automated checks (no hardware, no camera)
| # | Test | Setup | Duration | Telemetry / metric | Pass criteria | Artifact | Kind |
|---|---|---|---|---|---|---|---|
| A1 | Full ctest from a clean configure | fresh build dir, documented command | ~20 s | 12/12 suites | 0 failures | ctest log | automated |
| A2 | Chunked-parser conformance | `MjpegStub` in chunked mode | <5 s | pass/fail per case | all green | test log | automated |
| A3 | Reconnect ladder | stub closes socket N times | ~15 s | attempt count, delays | exactly 5 attempts at 0.5/1/2/3/5 s; counter survives connect | test log | automated |
| A4 | Malformed-input matrix | 6+ corrupt streams | <5 s | `fail()` reason | every case rejected, buffer cap enforced | test log | automated |
| A5 | 401/409/4xx config paths | control stub | <5 s | re-send count | 401 re-queued once; 409 retried once | test log | automated |
| A6 | Recorder crash/truncation | truncated container | <5 s | recovered frames | all pre-truncation frames byte-identical | test log | automated |
| A7 | Recording off the GUI thread | thread-id probe | <5 s | `QThread::currentThread()` | ≠ GUI thread | test log | automated |
| A8 | Profile-ladder provenance | data-driven assertion | <1 s | ladder fields | every figure traceable to a cited run | test log | automated |

### B. Semi-automated integration (fake camera, no hardware)
`tools/fake_camera.py` exists, is complete, and is wired to nothing. Start it, then:

| # | Scenario | Telemetry | Pass criteria |
|---|---|---|---|
| B1 | Full happy path: connect → auth → stream → snapshot → record → disconnect | app metrics + fake-camera access log | all endpoints exercised, 0 errors |
| B2 | Auth failure → lockout → recovery | HTTP status sequence | 401s until 8 failures, then 429-equivalent; recovers after TTL |
| B3 | Token expiry mid-session | 401 on status | app re-authenticates, no settings lost |
| B4 | Stream dropped at t, resumed | reconnect counters | bounded ladder, `frames_presented` resumes |
| B5 | Camera wedged (fake returns 500 `no sensor`) | recovery counters | self-repair requested, hint shown, then cleared |
| B6 | Malformed MJPEG from a "camera" | parser counters | rejected cleanly, no crash, no OOM |

### C. Live hardware tests (ESP32-CAM + OV2640 required)
| # | Scenario | Duration | Telemetry | Pass criteria |
|---|---|---|---|---|
| C1 | Startup → connect → stream → clean shutdown | 5 min | uptime monotonic, heap, fps | no reboot, 0 capture failures |
| C2 | Repeated connect/disconnect | 20 cycles | `reset_reason`, heap | 0 reboots, 0 spurious 409 (regression on ADR-0009 class) |
| C3 | **Camera reboot mid-stream** | 5 min | uptime discontinuity, recovery | bounded reconnect, `reset_reason` = expected, reconnect < 30 s |
| C4 | **PC Wi-Fi leave/rejoin** | 2 × 80 s | reconnect counters, fps | recovery within ladder, 0 corrupt |
| C5 | **Network interruption mid-recording** | 60 s outage | container integrity | container recoverable via `scan()`; pre-outage frames intact |
| C6 | **Sensor settings survive recovery** (CP-4 regression) | 10 min | `/api/v1/status` before/after | brightness/contrast/etc. unchanged after recovery |
| C7 | Resolution/quality change while streaming | 10 changes | 409 then success | config applied, stream resumes, no wedge |
| C8 | Profile switch (all 6 ladder profiles) | 15 min | fps, frame bytes, config echo | each profile reaches its own floor or is labelled under-floor |
| C9 | Snapshot byte identity | 20 snapshots | SHA-256 vs on-wire | all identical (extends `62ec6ed`) |
| C10 | Recording start/stop, short and long | 5 × 5 min | index, recovery | no UI stall (CP-3), sidecar consistent |
| C11 | Unsupported control | n/a | capabilities | control hidden/disabled, never faked (§20) |
| C12 | Multiple devices announced | 2 cameras | registry state | second announce does not steal the view; switching works |
| C13 | **Controlled scene A/B** (the open Phase 6 experiment) | 2 × 10 min | frame bytes, fps, `avg_capture_ms` | still scene vs changed scene isolates the scene-dependence claim |
| C14 | XCLK > 27 MHz probe | 28, 30 MHz | fps, recoveries, frame bytes | settle whether 27 MHz is a policy cap or a real ceiling |
| C15 | **Long recording** (1 h at SVGA q36) | 60 min | see D | no unbounded growth, no UI stall |

### D. Long-duration soak (the open §34 gate)
| # | Scenario | Duration | Telemetry | Pass criteria |
|---|---|---|---|---|
| D1 | **Continuous soak at production default** | **≥ 60 min** | device: uptime, free heap, free PSRAM, RSSI, `capture_failures`, `camera_recoveries`, `reset_reason` @ 2 s. App: decoded/presented/overwritten/decode-failures, `present_age_ms` p95, process CPU, working set. | uptime monotonic; heap drift < 50 KB with no sawtooth growth; PSRAM flat; presented fps == device capture fps within 1%; 0 recoveries; 0 decode failures; no UI stall |
| D2 | Soak with recording active | ≥ 60 min | as D1 + index size, GUI frame time | memory growth bounded; no GUI stall (the CP-3 regression) |
| D3 | Soak across a resolution change | ≥ 60 min | as D1 | no leak across the deinit/init cycle (CP-4 regression) |
| D4 | Soak across a **camera reboot** | ≥ 60 min | as D1 + recovery count | no leak, no compounding failure |

Reuse `benchmarks/phase5_pipeline.py`'s validity report verbatim — it already refuses a
run when any required counter is missing, which is the property that makes the earlier
293 s and 145 s runs trustworthy-but-short. **D1 is the gate that has never been run.**

### E. Release / deployment validation
| # | Check | Pass criteria |
|---|---|---|
| E1 | Clean clone → documented build → `ctest` | 12/12 (this is CP-1's regression test) |
| E2 | `package-release.ps1` from clean | builds, tests, packages, fails on a red suite |
| E3 | Package on a machine with no Qt | starts, connects, streams, records |
| E4 | Licence/notice present in the package | present (CP: currently absent) |
| E5 | Version consistency across CMake / exe / About / package name / firmware | one source of truth, all five agree |
| E6 | Artifacts stamped with their build commits | app and firmware each record their own |
| E7 | Deployed-build soak | one hour against the packaged build, not the dev tree |

**Note on automated-ness:** A1–A8 and B1–B6 are fully automatable and should be; C1–C15
are semi-automated (script drives, human triggers the physical event); D1–D4 are
unattended; E1–E7 are scripted. No new infrastructure is required — `phase5_pipeline.py`,
`fake_camera.py` and the 12 suites cover it.

---

## 10. Confirmed Defects

Consolidated list with severities. Detail and evidence in §6.

| ID | Sev | Title | Location |
|---|---|---|---|
| CP-1 | **P0** | Tests do not build from a clean clone | `desktop/CMakeLists.txt:90` |
| CP-2 | P1 | Balanced profile carries vga/q24's byte figure | `ProfileEngine.cpp:92` |
| CP-3 | P1 | Recording + frame bus run on the GUI thread, O(N²) index | `CameraDevice.cpp:170-178` |
| CP-4 | P1 | Recovery silently reverts all sensor controls | `camera.c:226-234` |
| CP-5 | P1 | `m_recoveriesTriggered` never reset | `MjpegClient.cpp:466` |
| CP-6 | P1 | 401 on config write silently drops the change | `DeviceStatus.cpp:249` |
| CP-7 | P1 | Reconnect ladder + watchdogs untested | `tests/` (absence) |
| CP-8 | P2 | Tested wire format ≠ shipped wire format | `tst_capture.cpp:59-88` |
| CP-9 | P2 | No malformed-input tests | `tests/` (absence) |
| CP-10 | P2 | Three status overlays at identical coordinates | `Main.qml:1737/1750/1763` |
| CP-11 | P2 | `releaseStale()` has no production caller | `DeviceRegistry.cpp:100-123` |
| CP-12 | P2 | `applyNextConfig` abandons options on a gap | `main.cpp:246-255` |
| CP-13 | P2 | Per-read recompute ≈ 600 wasted allocs/s | `SessionState.cpp:127` |
| CP-14 | P2 | Sensor sliders lose their binding on first drag | `Main.qml:1502` |
| CP-15 | P2 | Notifications dismissed by mutable index | `Main.qml:658` |
| CP-16 | P2 | HighQuality held to an invented provisional floor | `ProfileEngine.cpp:51-73` |
| CP-17 | P2 | One run presented as three | `ProfileEngine.cpp:68,73` |
| CP-18 | P2 | NVS restore skips enum validation | `camera.c:331-342` |
| CP-19 | P2 | Camera mutex held across network sends | `http_servers.c:364-370` |
| CP-20 | P2 | `app_version` not reported by the device | `docs/protocol.md:70` |
| CP-21 | P2 | No protocol-compatibility check on the desktop | `DiscoveryService.cpp:228` |
| CP-22 | P2 | Credential test writes to the real registry | `tst_credentialstore.cpp:20` |
| CP-23 | P2 | "drop" metric conflates decode failures and drops | `Main.qml:803-811` |
| CP-24 | P3 | Firmware version reaches the wire via IDF default | `http_servers.c:56` |
| CP-25 | P3 | "Conservative reading" rule not implemented | `ProfileEngine.cpp:174-192` |

## 11. Confirmed Gaps

| ID | Sev | Gap | Requirement |
|---|---|---|---|
| G-1 | P1 | **§28 hardware capability detection entirely absent** | §28, `deployment.md:12` |
| G-2 | P1 | **No licence/notice in the shipped package** — not redistributable | §29 |
| G-3 | P1 | **≥1 h soak gate never run** (longest: 293 s; one 145 s aborted) | §34, ADR-0010 |
| G-4 | P2 | No installer (zips only) | §40 |
| G-5 | P2 | `licensing.md` unclosed; dependency set understated | §29 |
| G-6 | P2 | No CI — nothing ever builds or runs the tests | §45 |
| G-7 | P2 | No root README | §31 |
| G-8 | P2 | `package-release.ps1` neither builds nor tests | §40, AGENTS.md |
| G-9 | P2 | Release script stamps the wrong commit as firmware provenance | §39/§13 |
| G-10 | P2 | Version split across four places, already inconsistent | §39 |
| G-11 | P2 | `docs/deployment.md` stale for a shipped release | §31 |
| G-12 | P2 | Zero `Accessible.*` properties in the QML | §25 |
| G-13 | P3 | `tools/fake_camera.py` (578 lines, 8 fault modes) wired to nothing | §33 |

---

## 12. Documentation Drift

| ID | Conflict | Sources | Authoritative | Why | Recommended update |
|---|---|---|---|---|---|
| DD-1 | **Transport: TCP framed vs MJPEG** | ADR-0007 + `protocol.md:3` + `architecture.md:24` say TCP primary; `architecture.md:113` says MJPEG selected; code has **no** TCP/UDP client; `Kconfig:5` disables the transport; release README says "MJPEG over HTTP" | **The code + release artifact** | No desktop TCP/UDP client has ever existed (`git log --all --diff-filter=A` shows no such file); the firmware path is compiled out by default | Rewrite ADR-0007's status to record that the decision was **not implemented** and that MJPEG is the shipped transport, with the measured basis preserved. Fix `architecture.md:24` to match `:113`. |
| DD-2 | **Network topology** | Master Prompt §5 (router) vs ADR-0006/AGENTS.md/source (softAP) | **ADR-0006** | Accepted, user-directed, verified in source, and all benchmarks were taken on the direct link | Amend Master Prompt §5 with a superseded-by note; record the one-network-per-camera scale-out consequence |
| DD-3 | **Thread model** | `architecture.md:125` "net thread writes [FrameBus]", `:160`; `:128` recorder on net thread — all false | **Source** | `MjpegClient` is never moved; `frameReady` is a queued connection to the GUI thread | Correct the table once CP-3 is fixed |
| DD-4 | **Control-server socket count** | `architecture.md:38` says `max_open_sockets=4` → 7; source has 6 → 9 | **Source** | `http_servers.c:624` | Correct the table; the 16-socket conclusion still holds with more headroom |
| DD-5 | **`architecture.md:3` status line** | "Phase 2 baseline implemented — hardware bring-up pending" | **Git history** | Nine Phase-6/7 commits and two tags exist | Update to current phase status |
| DD-6 | **`architecture.md:22-23` module names** | Lists `control_api`, `transport` as modules; source files are `http_servers.c`, `frame_transport.c` | Source | Naming drift | Align names |
| DD-7 | **HD as production default** | `benchmark-results.md:447` "HD/q12 is an experimental profile, **not** the production default… not yet chosen" vs ADR-0010 and `ProfileEngine.cpp:51` | **ADR-0010** (the owner's later decision) | ADR-0010 was accepted after that section was written | Add a forward-pointer in `benchmark-results.md:447` to ADR-0010 rather than editing the measurement |
| DD-8 | **`performance.md:3`** | "Status: targets defined; numbers pending Phase 3/4 measurement" | **Source** | Phases 3/4/5 are complete with published results | Update the status line; the body is still accurate |
| DD-9 | **`licensing.md:9`** | "ESP-IDF pin TBD (candidate v5.5.x or v6.0.x)" | **ADR-0005** | Pinned to v5.5.4 in 2026-09-23 and recorded in `install-idf.ps1:4` | Update |
| DD-10 | **`testing.md` coverage claims** | Claims `tst_credentialstore` covers wrong-password rejection and storage-unavailable fallback (neither tested); claims "byte identity **on the wire**" for a test that compares against the parser's own output | Source | Verified by inspection | Correct both claims; the underlying code paths (DPAPI failure) are genuinely untested |
| DD-11 | **`architecture.md:26` discovery port** | Says announce "rate-limited to one per 250 ms"; `protocol.md:186` says the same — consistent, but `architecture.md:8` calls the AP SSID "ESP32-CAM" while `protocol.md` never states the SSID | — | Minor | Add the SSID to `protocol.md` for completeness |
| DD-12 | **`docs/protocol.md:99`** | "Verified against firmware `0.1.0` / proto v1 on 2026-09-26" while the release is 0.2.0 | — | Benign but confusing given CP-24 | Note which release the observation belongs to |
| DD-13 | **Two identical Master Prompt files** | Em-dash (canonical per brief, but stale — last edited `fdab851`) and hyphen (last edited `f3258d5`); byte-identical after CRLF normalisation | Neither, long-term | A designated-canonical file that is not the maintained one will drift | Keep one; delete the other in a commit that says which |
| DD-14 | **`.gitignore` breadth** | `*.dll`, `*.exe`, `*.map` unanchored | — | Nothing is wrongly ignored today, but a future committed test-fixture binary would be silently skipped | Anchor to `/dist/` or use `**/build/**` patterns |

---

## 13. Benchmark Audit

### 13.1 Methodology quality — genuinely above standard

The project holds itself to a rule most projects do not, and mostly keeps it:

- **Every published figure traces to a raw artifact.** I verified the Phase 3 tables
  cell-by-cell against all 6 JSON files (2462/22.38, 2476/22.50, 1888/17.16, 1217/11.06,
  979/8.90, 1209/10.99 fps, with matching frames, bytes, throughput, drops, latency and
  CPU) — **all exact**. I verified the Phase 4 envelope (all 28 cells), the fb/grab cross
  (including the four `ESP_ERR_NO_MEM` DRAM rejections and the two `HTTP 500 no sensor`
  SCCB-wedge cells), and the confirm ladder (16.34 / 18.97 / 19.21 / 42.00 / 44.61 /
  44.93) — **all exact**, including the `final_ladder` records.
- **Invalid runs are labelled, not deleted.** `phase5-xclk-*-x24/x26/x27` carry
  `validity.clean = false, problems: ["app never started measuring (no frames)"]` and
  are reported as failures, not quietly omitted.
- **Aborted runs are named ABORTED.** `phase6-soak-20260928-hd-q12-ABORTED-145s.json`.
- **Harness bugs are recorded for honesty** (`benchmark-results.md:132-146`): the
  non-zlib-compatible CRC, the UDP double-counter producing "impossible" 24.9 M values,
  and the debug one-liners that produced garbage headers.
- **Unfavourable results are published**: fb2 losing at every Phase 5 point despite
  winning in Phase 4; the affinity A/B run once per variant with the caveat stated;
  the same operating point measuring 9.95/15.91/19.65 fps in one session.

### 13.2 What the evidence does and does not support

**The HD frame rate is scene-dependent, not a fixed number.** This is the Phase 6
finding and it is well supported. The bracketed measurement is decisive: over one
42 s window the camera counted 344 frames captured (8.19/s) and 343 delivered (8.17/s)
while the application presented 8.25 fps, with `frames_overwritten = 0`,
`stale_dropped = 0`, `capture_failures = 0`, `camera_recoveries = 0`. A raw `curl :81`
with no application running measured 7.99 fps — the same conclusion from the opposite
direction. Frame interval is linear in compressed size at ~2.2 ms/KiB (correlation −0.852
across seven runs), which is mechanically sensible: the OV2640 encodes in the sensor, so
a larger frame takes longer to produce.

**8–11 fps is NOT a proven hardware ceiling — and the project says so.** I checked, and
`benchmark-results.md:738-743` is explicit and correct: "**Not claimed: that 8–11 fps is
the ceiling of this sensor.** The ~11.2 fps figure is the largest value ever *observed*
… No run has demonstrated a limit." The distinction between measured maximum, estimated
ceiling, correlated behaviour and proven physical limit is maintained throughout. This
is exemplary and should be preserved in any future edit.

**The 4–5 fps report was not reproduced, and the report is honest about that.** The
documentation states the lowest figure recorded is 7.80 fps, that reaching 4–5 would
require ~90–110 KiB frames at HD/q12, and that **no run in the repository has recorded a
frame that large** — so that figure is a prediction, not evidence. It also identifies and
closes the one cause that would have produced exactly that symptom (the `setConfigQuery`
single-slot drop, fixed in `9beced1`), which is a genuinely good piece of debugging.

### 13.3 Weaknesses I found

| ID | Sev | Issue |
|---|---|---|
| B-1 | **P1** | **The "three D2 runs" are one run** (CP-17). `10.00/9.97/9.96` are `capture_fps`/`delivery_fps`/`app decoded_fps` from a single 120 s artifact. ADR-0012 calls it "the only ladder point with a three-run measurement." The agreement is evidence the PC keeps up, not repeatability — and repeatability is exactly what HD's provisional floor needs. |
| B-2 | **P1** | **The 11.14 / 11.25 / 11.26 fps "small-frame" runs have no retained artifact.** `benchmark-results.md:643-651` tabulates four such runs; a search of all 50 result files finds none above 11.012 except `phase5-hddef-hd-q24` (11.174, a different quality). The 45 s `curl` capture is explicitly "not retained" (`:658-660`). The entire "one configuration yields 11 fps and then 8 fps" comparison — the headline Phase 6 result — rests on the retained side only. The direction is well supported by the `2.2 ms/KiB` relation; the specific 11.26 figure is not reproducible. |
| B-3 | P2 | **Envelope and confirm use different framebuffer counts** (fb3 vs fb2), and Phase 5 then showed fb2 loses 7–15% at high-rate points. So `measuredFps` (envelope, fb3) and `altMeasuredFps` (confirm, fb2) are not two measurements of one thing. The ladder mixes bases. ADR-0012 acknowledges the gap is "the uncertainty" but the *cause* is a config difference, not only scene drift. |
| B-4 | P2 | **Affinity A/B is one run per variant** (stated as a caveat at `:587-590`). With an 8.5–11.0 spread, a −16.7% and a −22.5% conclusion from single runs is directionally sound but not tight. A second core0 run is cheap. |
| B-5 | P2 | **Phase 3 is single-run per cell** with 13% frame-size differences between the HTTP and TCP HD runs. The doc correctly refuses to rank them, but the ADR still selects a winner on that basis. |
| B-6 | P2 | **`phase6-soak` and `phase5-soak` both record `app: None`** — the two longest runs have **no PC-side data at all**. "4.9 min partial soak" is a device-only record. |
| B-7 | P3 | The 27 MHz XCLK cap is explicitly a **policy cap, not a hardware limit** (`:426-427`), and the curve was "still rising at 27 MHz". Probing 28–30 MHz is a cheap, high-information experiment (C14). |

### 13.4 Open benchmark gates

| Gate | Status | Evidence |
|---|---|---|
| **≥1 h soak** | **OPEN** | Longest completed: 293 s, device-only. One 145 s aborted (correctly named). Correctly deferred to Phase 8 in `benchmark-plan.md`, `testing.md` and `benchmark-results.md` — **no false claim is made anywhere**. |
| ≥15 fps at production default | **NOT MET, honestly** | ADR-0010 scopes HD to ≥7 provisional. The conflict with §34 is recorded, not papered over. |
| ≥15 / ≥20 fps below 800×600 | **MET** | SVGA 22.42 (and 33.62 at x27), VGA 22.51, QVGA 44.94 — all with artifacts. |
| Scene A/B control | **OPEN** | The missing experiment named at `:744-749`. |
| 27 MHz ceiling | **OPEN** | Policy cap; not probed above. |
| Physical scene→display latency | **NOT MEASURED** | Correctly stated as not measured (`benchmark-plan.md` latency table). |

---

## 14. Test Coverage Audit

**125 test functions, ~600 assertion sites, 12 suites, 12/12 green** (verified by me).
No suite contains a bare `QVERIFY(true)` or an assertion-free test. The pure-logic
suites are genuinely good — several assert a *rule* and then assert the *outcome*
independently of the mechanism, which is the right shape.

### Strong
`tst_sessionstate` (18 tests — exhaustive branch/priority coverage of a pure function,
including all five ordering traps), `tst_recorder` (SHA-256 byte identity, sidecar
threshold, truncated-tail recovery), `tst_discovery` (real parse/dedupe/ageing),
`tst_profileengine` (ordering, the "only HD may go below 15" rule, three-window rule),
`tst_notificationcenter`, `tst_userprefs` (correct `QTemporaryDir` redirection),
`tst_devicestatus` (the best-written suite — real thread, real HTTP, "no stray request"
assertions), `tst_credentialstore` (real DPAPI), `tst_authclient` (I independently
reproduced its PBKDF2 vector under Python `hashlib` — it is a true independent
reference, not self-consistent).

### Weak / misleading
| Suite | Verdict | Why |
|---|---|---|
| `tst_capture` | **WEAK** | Real classes and real bytes, but only the happy path, the *identity* (non-chunked) transport, and no malformed input, no reconnect, no drop-under-load through the real pipeline. The live test is QSKIP-gated. |
| `tst_diagnostics` | **WEAK** | Every test constructs its own `Facility`, racing a process-wide message handler for buffer contents while asserting exact counts. The suppression logic itself is genuinely pinned. |

### Missing (the gaps that matter)
- Reconnect ladder, watchdogs, no-response recovery — **no test at all** (CP-7).
- Chunked decoding — the state machine the real device uses on every frame (CP-8).
- Malformed/truncated/oversized input — six `fail()` branches dead to CI (CP-9).
- The `Authorization: Bearer` header on the control plane — `tst_devicestatus` has an
  `authRequired` flag (`:47`) and **no test ever sets it** (`:133`).
- ADR-0012's stop→wait→config→reconnect — lives in QML, unreachable from the test target;
  the `stream_clients > 0` branch is never taken because the stub always reports 0.
- `FrameBus` under the real pipeline — `FrameImageProvider.cpp` is in the executable
  target, not `scamcore`, so no test can link it. Only direct `setFrame()` calls are tested.
- `AppMetrics` entirely (receive gaps, parse/decode µs, part counts).
- Snapshot error paths — unwritable directory, short write, disk full.
- The live byte-identity test proves identity **with the parser**, not with the wire: its
  only wire-side checks are `raw.size() > 1024` and SOI/EOI, and the re-encode
  comparison would pass for a file re-encoded at any quality other than 90. `testing.md:63`
  calls this "byte identity **on the wire**" — that overstates it.

### Environment-gated
`SCAM_TEST_HOST` + `SCAM_TEST_PASSWORD` (authclient, capture), `SCAM_DISCOVERY_PORT`
(discovery). All `QSKIP` with a message naming the variable — hermetic by default, which
is correct, but it means the three live suites never run in an automated pass.

---

## 15. Performance Risks

| ID | Sev | Risk | Detail |
|---|---|---|---|
| PR-1 | **P1** | **GUI-thread recording degradation** (CP-3) | O(N²/60) index rebuilds during exactly the operation a user deliberately loads the system with. Unmeasured — no recording has been benchmarked end-to-end. ADR-0011 itself says "recording overhead still needs a measured baseline before it is claimed to be negligible." |
| PR-2 | P2 | **§34 frame-rate gate unmet at the production default** | 8–11 fps vs a 15 fps target. Correctly recorded, owner-decided, floor-scoped. Not a defect — but it *is* an open acceptance criterion, and no profile currently combines HD pixels with ≥15 fps. |
| PR-3 | P2 | **HD frame rate varies ~40% with scene** | 7.80–11.26 fps at one configuration. A user can see a 3 fps difference with nothing changed. The docs handle this honestly; the UI does not currently surface any indication. |
| PR-4 | P2 | **Recording overhead unmeasured** | Never benchmarked. `main.qml:803-811` counts "drops" that conflate decode failures with bus overwrites (CP-23), so the drop counter is not a clean signal. |
| PR-5 | P3 | **`statsChanged` emitted per read** (CP-13) | ~600 wasted allocations/s on the GUI thread. Small, but on the same path as PR-1. |
| PR-6 | P3 | **`AppMetrics::snapshot()` sorts under the global mutex** | A bench-time `writeJson` blocks frame delivery. Only affects bench mode. |

**Not claimed anywhere, correctly:** 30 fps, zero-copy, hardware-accelerated decode or
rendering, "negligible" recording overhead, zero latency, production stability. I checked
for each. The project honours §34's ban.

---

## 16. Security / Licensing / Deployment Findings

### Security — sound
Control-plane auth is a real construction, not a placeholder: PBKDF2-HMAC-SHA256, salt
from `esp_fill_random`, constant-time `ct_equal` token compare, single-use nonces with
TTL, sliding token expiry, per-IP exponential backoff and an 8-failure 5-minute lockout,
bounded token table. Secrets come from a correctly git-ignored `config_secrets.h`. **I
grepped every `qDebug`/`qInfo`/`qWarning` in `desktop/src` and `main.cpp` for
password/token/verifier/proof/secret/Bearer: zero hits.** DPAPI is real
(`CryptProtectData` + `SecureZeroMemory` + `LocalFree`, `crypt32` linked), with an
empty-password guard and an undecryptable-blob handler.

| ID | Sev | Finding |
|---|---|---|
| S-1 | P2 | **PBKDF2 at 8192 iterations** (`auth.c:28`) — roughly two orders of magnitude below current guidance (~600k for PBKDF2-SHA256). On a softAP whose only adversary is a joined client, practical risk is low; recorded as a known bound, not a blocker. Note the desktop client must match the iteration count — a bump needs both sides. |
| S-2 | P2 | **No timestamp/replay window on state-changing control requests**, though `security.md:4` specifies "±30 s window and token binding." Nonce single-use covers the *login*; a captured authenticated request is replayable until the token expires. On a trusted LAN this is an accepted residual, but the doc currently claims more than the code does. |
| S-3 | P3 | **`client_slot()` evicts by fewest-fails** (`auth.c:146-160`) — normally the legitimate client's `fails == 0` entry — clearing its backoff. Only 4 slots, `AP_MAX_CLIENTS 4`. Low impact; wrong policy. |
| S-4 | P3 | **`savePassword` never zeroes its plaintext copy** (`CredentialStore.cpp:115`) while `loadPassword` does. Defence-in-depth asymmetry, not a leak. |
| S-5 | — | Video stream is unauthenticated by design (§19, ADR-0006). Correctly scoped and documented. |

### Licensing — mechanism correct, obligations unmet
Dynamic linking is real and Qt modules are LGPL-safe. `package-release.ps1:59-73`
actively **enforces** this by objdump-ing the exe's import table and throwing on any
GPL-tier module — better than most projects do. **But:**

- **G-2 (P1): the shipped zip contains no LICENSE, NOTICE, COPYING or attribution** — 0
  of 1,388 entries. The LGPLv3 §4 notice/relinking obligations are unmet, so the zip is
  **not redistributable as-is**. This is the single most consequential release blocker.
- **G-5 (P2): `licensing.md` is unclosed and understates the dependency set.**
  `QuickControls2` is linked but unlisted; `Multimedia` is listed but not linked; ~18
  shipped Qt modules are unrecorded; all four pre-architecture-lock checkboxes unchecked.

### Deployment — substantial, but unattended
The produced artifact genuinely runs without Qt. `windeployqt` layout, `qwindows.dll`
asserted, three MinGW runtime DLLs copied, `opengl32sw.dll` present, QML compiled into
the exe resource. **But the pipeline never builds and never tests** (G-8), stamps the
packaging commit rather than the build commit as firmware provenance (G-9), and there is
no installer (G-4), no CI (G-6) and no README (G-7).

---

## 17. MIMO IMPLEMENTATION BACKLOG

Confirmed findings only. Suspected issues are in §18.

### Group A — Must-fix before Phase 7 continues

**Task A1 — Make the test suite build from a clean clone**
- **Priority:** P0 · **Fixes:** CP-1
- **Problem:** `if(BUILD_TESTING)` guards a variable CMake never defines; `include(CTest)` is absent repo-wide.
- **Evidence:** documented configure → 0 `tst_` targets, no `CTestTestfile.cmake`; `-DBUILD_TESTING=ON` → 60 targets. Proved both ways.
- **Files:** `desktop/CMakeLists.txt:88-93`
- **Symbols:** `project()`, `if(BUILD_TESTING)`, `enable_testing()`
- **Required behaviour:** a fresh clone following `AGENTS.md` builds and registers all 12 suites.
- **Minimal strategy:** add `include(CTest)` before the guard; hoist `enable_testing()` out of the conditional. (Deleting the guard is also acceptable — `tests/` is cheap.)
- **Do not change:** `tests/CMakeLists.txt` suite definitions.
- **Regression test:** configure a throwaway dir with the documented command; assert `ctest -N` reports 12.
- **Build/test command:** `cmake -S desktop -B <fresh> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\mingw_64` then `ctest`.
- **Benchmark requirement:** none.
- **Documentation:** `AGENTS.md` and `docs/testing.md` build snippets become true as written; add `-DBUILD_TESTING=ON` only if the guard is kept.
- **Acceptance:** 12/12 from a clean configure; this is also the E1 regression test.
- **Risk/rollback:** one line; trivially revertible.
- **Depends on:** nothing. Do this first.

**Task A2 — Correct the Balanced profile's byte figure**
- **Priority:** P1 · **Fixes:** CP-2
- **Problem:** `medianBytes = 14680.0` on svga/q24 is vga/q24's p50.
- **Evidence:** exhaustive search — 14,680 occurs in exactly one cell, vga/q24/fb2/confirm. Real svga/q24: 16,982 B (envelope fb3), 23,077 B (confirm fb2).
- **Files:** `desktop/src/ProfileEngine.cpp:92`
- **Symbols:** `ProfileEngine::buildLadder()`
- **Required behaviour:** every published figure is measured on the configuration it describes.
- **Minimal strategy:** use 16,982 (the source of `measuredFps = 22.50`), or 0 with an extended evidence string.
- **Do not change:** the ladder ordering or the floors.
- **Regression test:** assert no two ladder entries share `medianBytes` across different resolutions; assert every non-zero figure appears in a cited artifact.
- **Benchmark requirement:** none — this removes a claim, it does not add one.
- **Documentation:** none beyond the code comment.
- **Acceptance:** no ladder figure lacks a traceable source cell.
- **Risk/rollback:** display-only value; low risk.
- **Depends on:** A1 (so the test can run).

**Task A3 — Move recording and the frame bus off the GUI thread**
- **Priority:** P1 · **Fixes:** CP-3
- **Problem:** `frameReady` lands on the GUI thread; `Recorder::appendFrame` does two unbuffered writes per frame and a full O(N) index rebuild every 60 frames.
- **Evidence:** `MjpegClient` ctor moves only the worker (`:423-429`); `CameraDevice.cpp:170-178` connects to the client, not the worker. `Recorder.cpp:103` `Unbuffered`; `:172-175` `writeIndex()`.
- **Files:** `desktop/src/CameraDevice.cpp`, `Recorder.{h,cpp}`, `MjpegClient.{h,cpp}`
- **Symbols:** `CameraDevice` ctor, `Recorder::appendFrame`, `Recorder::writeIndex`, `MjpegWorker::frameReady`
- **Required behaviour:** frame persistence must not execute on the GUI thread; the recorder must still observe the exact bytes of the frame the bus holds.
- **Minimal strategy:** move the `frameReady` connection to an object living on the net thread, or give `Recorder` a worker with a bounded latest-frame queue. Pass image+raw in one signal (the signature already does) to preserve the "snapshot is the picture on screen" guarantee. Make `writeIndex()` incremental, or move it off the write path.
- **Do not change:** the `.scamrec` format (ADR-0011); the 60-frame flush cadence; `Scan` recovery semantics.
- **Regression tests:** recorder runs off the GUI thread; existing `tst_recorder` byte-identity, sidecar and truncation tests stay green; snapshot identity test stays green.
- **Build/test command:** standard build + `ctest`.
- **Benchmark requirement:** measure a 10-minute SVGA q36 recording and report GUI frame time p95. This is the first recording-overhead measurement the project has.
- **Documentation:** `docs/architecture.md:125,128,160` become true; ADR-0011's "recording overhead still needs a measured baseline" can then be closed.
- **Acceptance:** no GUI stall during a long recording; presented fps equals device fps while recording.
- **Risk/rollback:** touches the most delicate path in the app. Land behind the existing tests; revertible but re-verify byte identity carefully.
- **Depends on:** A1.

**Task A4 — Make camera recovery preserve sensor controls**
- **Priority:** P1 · **Fixes:** CP-4
- **Problem:** every `camera_driver_init()` resets nine sensor controls to defaults; NVS restore runs only at boot.
- **Evidence:** `camera.c:226-234`; `camera_control_init` has exactly one call site (`app_main.c:113`).
- **Files:** `firmware/esp32_cam_stream/main/camera.c`, `camera_control.{c,h}`, `app_main.c`
- **Symbols:** `camera_driver_init`, `camera_init`, `camera_recover`, `camera_control_init`
- **Required behaviour:** the operating point **and** the image settings survive any re-init.
- **Minimal strategy:** extract the default-setting block into an explicit `apply_sensor_defaults()` called only on first boot, and call `camera_control_restore()` after every successful init.
- **Do not change:** the frame budget, the recovery ladder, NVS schema (no migration needed — the data is already stored).
- **Regression test:** live-hardware (C6) — set a non-default brightness, force recovery, assert it survives and `/api/v1/status` reports it.
- **Build/test command:** `idf.py build`; flash and run C6.
- **Benchmark requirement:** confirm HD/q12 frame rate is unchanged after the refactor (the block moves, it does not change).
- **Documentation:** ADR-0009 gains a note that sensor controls join the operating point in NVS persistence.
- **Acceptance:** reboot and recovery both preserve all sensor controls.
- **Risk/rollback:** firmware change on the recovery path. Verify the boot path still applies defaults on a factory-fresh device.
- **Depends on:** A1 for the harness; otherwise independent.

### Group B — Must-fix during Phase 7

**Task B1 — Reset the recovery counter and re-queue dropped config writes**
- **Priority:** P1 · **Fixes:** CP-5, CP-6
- **Problem:** `m_recoveriesTriggered` is never reset, so self-repair is permanently disabled after two uses; a 401 on a config write discards the user's change while announcing a retry that never happens.
- **Evidence:** `MjpegClient.cpp:466-467` (3 occurrences, no reset; contrast `m_noResponseStreak` at `:497`); `DeviceStatus.cpp:249-252` with `m_pendingKv.clear()` at `:924`.
- **Files:** `desktop/src/MjpegClient.cpp`, `DeviceStatus.cpp`
- **Symbols:** `MjpegWorker::errorOccurred` handler, `DeviceStatus::trySendPendingConfig`
- **Required behaviour:** self-repair re-arms after a healthy period; an authenticated session expiry must not lose a settings change.
- **Minimal strategy:** reset the counter alongside `m_noResponseStreak` and in `start()`; on 401, retain/re-queue `m_pendingKv` and re-send on `signIn` completion.
- **Do not change:** the 5-step ladder, the 409 single-retry, the 8-failure lockout contract.
- **Regression test:** `tst_devicestatus` — stub returns 401 once then 200; assert exactly one re-send and that the value lands. `tst_capture`/live — assert a recovery request is issued again after a clean run.
- **Acceptance:** no lost settings on token expiry; self-repair available after any healthy period.
- **Risk/rollback:** the 401 path must not become an infinite retry loop — bound it exactly as the 409 path is bounded.
- **Depends on:** A1.

**Task B2 — Close the test gaps where the risk is**
- **Priority:** P1 · **Fixes:** CP-7, CP-8, CP-9, and the §33 gaps in §14
- **Problem:** the reconnect ladder, the chunked decoder the firmware actually speaks, and every malformed-input branch are untested.
- **Evidence:** no test drives a socket close, a stall, or a chunked stream; `tst_capture.cpp:59-88` sends no `Transfer-Encoding` while `http_servers.c:364-369` uses `httpd_resp_send_chunk`.
- **Files:** `tests/tst_capture.cpp`, `tests/CMakeLists.txt`, possibly `tests/tst_devicestatus.cpp`
- **Required behaviour:** the shipped wire format, the reconnect contract and the input bounds are all covered.
- **Minimal strategy:** add a `chunked` flag to `MjpegStub`; add a reconnect test asserting attempt count, exact delays and that a TCP connect does not reset the counter; add malformed cases; enable `authRequired` in one `tst_devicestatus` test.
- **Do not change:** production code unless a test finds a real bug.
- **Acceptance:** the chunked decoder, the ladder and the bounds all have green tests.
- **Risk/rollback:** test-only; low risk. The highest-value use of `tools/fake_camera.py` (see INV-1).
- **Depends on:** A1.

**Task B3 — Fix the QML correctness defects**
- **Priority:** P2 · **Fixes:** CP-10, CP-14, CP-15, CP-23
- **Problem:** three overlays share identical anchors; sensor sliders lose their binding on first drag; notifications are dismissed by an index that `postOnce` invalidates; the "drop" metric conflates decode failures with drops.
- **Evidence:** `Main.qml:1737/1750/1763` (all `anchors.margins: 14`); `:1502` `value: parent.current` vs the quality slider's `syncFromStatus()` at `:1223-1227`; `:658` `dismiss(index)` vs `NotificationCenter.cpp:108-124`; `MjpegClient.cpp:393-396`.
- **Files:** `desktop/qml/Main.qml`, `desktop/src/NotificationCenter.cpp`
- **Symbols:** three bottom-anchored `Label`s; sensor `Repeater` delegate `Slider`; `notifyCard`; `NotificationCenter::data()`
- **Required behaviour:** no two messages occupy the same pixels; sliders keep tracking status; the dismissed card is the clicked card; the drop metric means what it says.
- **Minimal strategy:** distinct `anchors.bottomMargin` per overlay; give sensor sliders the same guarded sync; add a key role to `data()` and switch QML to `dismissKey()`; rename or fix the footer metric.
- **Do not change:** the `postOnce` C++ behaviour fixed in `9beced1` — this is the QML-side counterpart.
- **Regression test:** manual UI-automation pass as `9beced1` did; consider moving `FrameImageProvider.cpp` into `scamcore` to make the display path testable.
- **Acceptance:** no overlap in any state combination; slider tracks after a drag.
- **Risk/rollback:** low; visual.
- **Depends on:** A1 for any new automated coverage.

**Task B4 — Add §28 hardware capability detection**
- **Priority:** P1 (gap) · **Fixes:** G-1
- **Problem:** no CPU/RAM/graphics-backend/screen/touch detection anywhere.
- **Evidence:** `grep QSysInfo|QScreen|opengl|graphicsApi|rhi|touch` over `desktop/src` and `main.cpp` returns no detection code.
- **Files:** new `desktop/src/Capabilities.{h,cpp}`, `main.cpp`, diagnostics panel
- **Required behaviour:** the app determines and reports the capabilities §28 lists, with safe fallbacks.
- **Minimal strategy:** detection and display only, surfaced in the existing F12 panel — no new UI surface, no behaviour branch in this step.
- **Do not change:** the rendering path; no capability-based behaviour until a later step.
- **Regression test:** the probe returns sane values on the dev machine.
- **Documentation:** `docs/deployment.md:12` becomes true; `architecture.md` gains a capability section.
- **Acceptance:** every §28 bullet is answered in the diagnostics panel.
- **Risk/rollback:** additive, read-only.
- **Depends on:** nothing.

**Task B5 — Productize packaging, licensing and versioning**
- **Priority:** P1 (G-2) / P2 (rest) · **Fixes:** G-2, G-4, G-5, G-8, G-9, G-10, G-11, CP-24
- **Problem:** the release zip carries no licence notice and so is not redistributable; the release script never builds or tests; version identity is split across four disagreeing places; artifacts record the wrong provenance.
- **Evidence:** 0 licence entries in 1,388 zip entries; `grep ctest scripts/` → 0 hits; `CMakeLists.txt:3` 0.1.0 vs `main.cpp:48` 0.2.0 vs manifest `0.1.0.0` vs package name from firmware `FW_VERSION`; `package-release.ps1:29-34` freshness-checks only the exe against `main.cpp`; firmware README records `git rev-parse HEAD`.
- **Files:** `scripts/package-release.ps1`, `desktop/CMakeLists.txt`, `desktop/main.cpp`, `firmware/esp32_cam_stream/CMakeLists.txt`, `docs/licensing.md`, `docs/deployment.md`
- **Required behaviour:** a clean clone produces a built, tested, correctly-versioned, correctly-labelled, redistributable package whose artifacts each record their own build commit.
- **Minimal strategy:** (1) `cmake --build` + `ctest` before staging, failing on a red suite; rebuild or freshness-check the firmware too; (2) ship a `LICENSE`/`NOTICE`; (3) one version source threaded to CMake, the exe, About, the package name and the firmware; (4) complete `licensing.md` with the real module list and close the checkboxes; (5) per-artifact build commit; (6) set the firmware `project(... VERSION ...)`; (7) rewrite `docs/deployment.md` and reference the script.
- **Do not change:** the LGPL dynamic-linking layout or the GPL-module guard at `:59-73` — both are correct and should be preserved verbatim.
- **Regression test:** E2–E7 in §9E.
- **Documentation:** `licensing.md`, `deployment.md` rewritten; the four checkboxes closed.
- **Acceptance:** clean clone → `package-release.ps1` → a tested, redistributable, correctly-labelled package.
- **Risk/rollback:** the version unification touches build config; do it in its own commit.
- **Depends on:** A1 (the script must be able to run tests).

**Task B6 — Correct the HighQuality floor and the D2 evidence string**
- **Priority:** P2 · **Fixes:** CP-16, CP-17
- **Problem:** HD q12 ships as "the production default" on a provisional ≥7 floor that the benchmark document explicitly declines to settle; and "three D2 runs" is one run.
- **Evidence:** `benchmark-results.md:447-457` vs `ProfileEngine.cpp:51-73`; the artifact has one run with `capture_fps`/`delivery_fps`/`app decoded_fps` = 10.0/9.966/9.958 and mean frame 28,956 B, not "three runs at 58 KB."
- **Files:** `desktop/src/ProfileEngine.cpp:51-73`, `docs/decisions/0012`, `docs/benchmark-results.md`
- **Required behaviour:** provisional numbers are labelled provisional; a measurement is described as what it is.
- **Minimal strategy:** correct the comment and evidence string to say "one 120 s run, three counters" and "28,956 B"; mark the ≥7 floor provisional in the UI hint as ADR-0010 does; add the ADR-0010 forward-pointer at `benchmark-results.md:447`.
- **Do not change:** the owner's decision that HD is the default; the floor value itself (that is a Phase 8 outcome).
- **Acceptance:** no provisional figure is presented as settled; the D2 claim matches the artifact.
- **Risk/rollback:** comment/text only.
- **Depends on:** A2.

### Group C — Must-fix before Phase 8

**Task C1 — Report `app_version` from the device, or correct the doc**
- **Priority:** P2 · **Fixes:** CP-20
- **Evidence:** `protocol.md:70` claims it; `http_servers.c:56-58` and `discovery.c:58-59` do not send it; no firmware path reads it.
- **Minimal strategy:** the doc is the thing that is wrong — a device cannot know the app version unless sent. Correct the sentence; the recording header already carries it.
- **Acceptance:** `protocol.md` matches the code.

**Task C2 — Add a protocol-compatibility check**
- **Priority:** P2 · **Fixes:** CP-21
- **Evidence:** `DiscoveryService.cpp:228` parses it; no `kProtoVersion` and no comparison exist in `desktop/`.
- **Minimal strategy:** one constant, one compare, one clear UI error — exactly what `protocol.md:69` already promises.
- **Acceptance:** a future firmware with a different major version is rejected with a stated reason.
- **Risk/rollback:** must not lock out a legitimate minor bump; compare major only, per the doc.

**Task C3 — Call `releaseStale()`, or document the unbounded-growth seam**
- **Priority:** P2 · **Fixes:** CP-11
- **Evidence:** only test callers; every new `acquire()` builds two threads and two timers.
- **Minimal strategy:** call it from the discovery-ageing path in `main.cpp`, or document the bound and add a cap.
- **Acceptance:** a device announcing with rotating ids does not grow threads without bound.
- **Depends on:** careful teardown review — `~CameraDevice` uses `BlockingQueuedConnection` + `wait()` (DX-17), so calling this from the GUI thread will block the GUI. **Do not call it from the GUI thread without first moving teardown off it.**

**Task C4 — Validate enums on NVS restore**
- **Priority:** P2 · **Fixes:** CP-18
- **Evidence:** `camera.c:331-336` range-checks four fields; `:341-342` casts two enums unchecked, while the API path validates both at `:444-445`.
- **Minimal strategy:** extend the existing range check to `grab_mode` and `fb_location`, mirroring `:444-445`.
- **Acceptance:** an out-of-range stored enum falls back to defaults with a log line, like the other four.
- **Risk/rollback:** trivial; reuses an existing pattern.

**Task C5 — Do not hold the camera mutex across network sends**
- **Priority:** P2 · **Fixes:** CP-19
- **Evidence:** `http_servers.c:364-370` sends three chunks between `fb_get` and `fb_return`; `send_wait_timeout` is unset so ESP-IDF's 5 s default applies.
- **Minimal strategy:** copy the frame into a bounded PSRAM buffer, release the camera lock, then send. **Measure first** — ADR-0010 already redirected Phase 5 away from copy-reduction for the same reason, and adding a copy could cost frame rate. Alternative with no copy: set an explicit short `send_wait_timeout` so a stalled client cannot pin the lock.
- **Acceptance:** a paused client cannot stall the camera or the watchdog for seconds.
- **Benchmark requirement:** re-run the HD/q12 XCLK-18 point before/after; fps must not regress.
- **Risk/rollback:** touching the hot path. **Measure, do not assume.**

**Task C6 — Fix the smaller desktop defects**
- **Priority:** P2/P3 · **Fixes:** CP-12, CP-13, CP-22, CP-23, CP-25
- **Minimal strategy:** advance `configSlot` on the `!parser.isSet()` branch; throttle `statsUpdated` to ~4 Hz; redirect `QSettings` in `tst_credentialstore` to a temp dir; fix/rename the drop metric; implement the documented conservative-reading rule in `ProfileEngine`.
- **Acceptance:** a partial `--bench` option set applies every option that was given.

**Task C7 — Repository hygiene**
- **Priority:** P2/P3 · **Fixes:** G-6, G-7, G-11, DD-13, DD-14
- **Minimal strategy:** root `README.md`; a minimal CI job (configure → build → `ctest`); collapse the duplicate Master Prompt; anchor `.gitignore` binary patterns.
- **Acceptance:** a new agent builds, tests, runs and packages from the README alone.

### Group D — Phase 8 validation-only
These are not fixes; they are the Phase 8 program in §9. The single mandatory gate is
**D1 (≥1 h soak)** — open since Phase 5, with the longest completed run at 293 s and one
correctly-labelled 145 s abort. Also note that **every phase-5/6 soak artifact has
`app: None`** (B-6), so the gate must be designed to capture PC-side telemetry too.

### Group E — Optional P2/P3
PBKDF2 iteration count (S-1) with a documented bound; a replay/timestamp window or a
corrected `security.md` claim (S-2); `client_slot` eviction policy (S-3); zeroing the
plaintext copy in `savePassword` (S-4); the 900 ms heuristic timer before the real
`stream_clients == 0` barrier; `main.cpp:42-43` dead `qmlRegisterUncreatableType`;
`CMAKE_CXX_STANDARD 17` vs the C++20 project description; `main.cpp:100-101` unvalidated
`--bench` integer; `ProfileEngine.cpp:480` unguarded null deref in `advice()`; the
misindented `Label` at `Main.qml:812-814`; windeployqt shipping 6 unused QuickControls2
styles (~98 MB staged).

---

## 18. MIMO INVESTIGATION BACKLOG

Suspected or unresolved. **These are questions, not confirmed defects.** Do not fix them
as if they were.

| ID | Question | Verification method | Priority |
|---|---|---|---|
| INV-1 | Is `tools/fake_camera.py` intended as a CI fixture or a manual tool? It emulates the full control API, the real chunked wire format, the real auth scheme and 8 fault injections — and is referenced by nothing. | Ask the author. If intended for automation, it is the natural backbone for Task B2 and scenarios B1–B6; if manual, document it and wire it into `docs/testing.md`. | High |
| INV-2 | **Why did the scene change?** The 11 fps → 8 fps shift on 2026-09-29 was never explained. `benchmark-results.md:744-749` names the controlled A/B as the missing experiment. | Run C13: identical configuration, scene deliberately held still for 10 min then deliberately changed for 10 min, recording frame bytes + fps + `avg_capture_ms` at 2 s. | High |
| INV-3 | **Can 4–5 fps actually occur?** The owner reported it; the lowest recorded is 7.80. The model predicts 4–5 fps needs ~90–110 KiB frames, which no run has produced at HD/q12. | Point the camera at a high-detail scene at HD/q12 and record the frame byte count in the footer. If a frame ≥90 KiB appears, the model is confirmed and the "ceiling" claim needs revisiting. | High |
| INV-4 | **Is 27 MHz a real ceiling or just a policy cap?** The SVGA curve was "still rising at 27 MHz"; `xclk` validation stops at 27. | Run C14 at 28 and 30 MHz on SVGA q36. Watch for `FB-OVF`, frame inflation and recovery counts as at HD 24 MHz. | Medium |
| INV-5 | Does the deployed render loop ever call `requestImage` off the GUI thread? `FrameImageProvider`'s resolver reads `registry.activeDevice()` with no synchronisation while `setActive` writes it. `asynchronous: false` suggests no; the Qt contract permits it. | Log the thread id on the first `requestImage` call in a real deployment, or inspect `QQuickWindow::sceneGraphBackend`/`QSG_RHI_BACKEND` at runtime. If it is ever off-thread, add an atomic. | Medium |
| INV-6 | Can `ProfileEngine::activeIndex()`'s `ladder().size() + 1` "Custom" sentinel be dereferenced by a future caller? Today every consumer bounds-checks. | Grep for future uses; consider a named `CustomIndex` constant. | Low |
| INV-7 | Is the 900 ms timer before the profile mid-stream config reachable as a race, or is it a harmless heuristic? `DeviceStatus` independently waits for `stream_clients == 0` for up to 8 s, which is the real barrier. | Instrument and confirm the C++ barrier always fires first. If the timer is never the binding constraint, remove it. | Low |
| INV-8 | Does `main.cpp:283`'s fallback timer (`1200 + 2500 × (configCount + 2)`) force-start the stream in a way that can produce a firmware 409? | Trace whether it can fire while `configBusy`; if so, confirm the firmware's 409 retry covers it. | Low |
| INV-9 | Are the two full build trees under `desktop/` (`build/` and `build/Desktop_Qt_6_11_2_MinGW_64_bit_Debug/`) tracked or ignored? | `git check-ignore` on both. If tracked, they have been masking A1 in CI. | Low |
| INV-10 | What is the 4–5 fps report's actual frame size? §"Interpretation" says "the frame byte count in the footer is the one number that settles it." | Ask the owner for the footer reading from the failing session. | Medium |

---

## 19. Final Acceptance Gates

### Must close before Phase 7 is called done
| # | Gate | Currently |
|---|---|---|
| 1 | Clean clone → documented build → 12/12 tests | ✗ **fails** (CP-1) |
| 2 | Every ladder figure traceable to a raw artifact | ✗ (CP-2, CP-17) |
| 3 | GUI thread free of blocking work, including recording | ✗ (CP-3) |
| 4 | Camera recovery preserves the full operating point | ✗ (CP-4) |
| 5 | No settings silently lost on session expiry | ✗ (CP-6) |
| 6 | Reconnect contract covered by a test | ✗ (CP-7) |
| 7 | §28 capability detection present | ✗ (G-1) |
| 8 | Package is redistributable (licence/notice present) | ✗ (G-2) |
| 9 | Release script builds and tests | ✗ (G-8) |
| 10 | Version identity consistent across CMake/exe/About/package/firmware | ✗ (G-10) |

### Must close before Phase 8 is called done
| # | Gate | Currently |
|---|---|---|
| 11 | **≥1 h soak at the production default, with PC-side telemetry** | ✗ open (G-3); longest 293 s, device-only |
| 12 | Camera reboot, PC Wi-Fi leave/rejoin, stream interruption | ✗ not run against the final architecture |
| 13 | Truncated-recording recovery under real interruption | ✗ (tested only synthetically) |
| 14 | Recording overhead measured | ✗ never measured (PR-4) |
| 15 | Multiple devices discovered + active-device switching | ✗ structurally tested only |
| 16 | Long recording with no GUI stall | ✗ (depends on A3) |
| 17 | Deployed-build validation (E3, E7) | ✗ |
| 18 | Protocol-compatibility rejection | ✗ (CP-21) |
| 19 | §34 acceptance published honestly, including the unmet ≥15 fps at HD | ✓ **already done** — this is the gate the project has honoured best |
| 20 | Scene A/B experiment (INV-2) and 27 MHz probe (INV-4) | ✗ open |

### Explicitly *not* required
- Second simultaneous camera view (§27 permits one active stream).
- mDNS discovery (deliberately replaced for the softAP link).
- TLS on the video plane (§19 excludes it).
- OTA updates (`deployment.md` scopes them out until a partition strategy exists).

---

## 20. Executive Handoff Summary

**What you are inheriting.** A project whose *engineering* is ahead of its *record-keeping*.
The firmware recovery model, the measurement discipline, and the desktop's threading
discipline are the work of someone who understands what they are doing — Phase 3 and
Phase 4 numbers reproduce exactly from raw artifacts, invalid runs are labelled rather
than deleted, and `benchmark-results.md` maintains the measured/correlation/hypothesis
distinction that most projects abandon by Phase 3. Two commits ago, the previous agent
ran the built application under UI automation, found five real defects, and fixed each
with a test where one was possible. That is the right process and it should continue.

**What is actually wrong.** Nothing on the live video path. The stream is bounded,
counted, measured and honest. The defects are concentrated in three places:

1. **The safety net does not deploy.** A clean clone following the documented commands
   builds **zero** tests. Every claim of "12/12 green" depends on a flag nobody wrote
   down. Until A1 lands, no other fix in this document can be protected by a regression
   test, and the next agent will not know that.
2. **Two shipped numbers are not what they claim.** The Balanced profile's byte figure
   is a different resolution's measurement — the exact thing `ProfileEngine.h` warns
   against, 200 lines above the line that does it. And "three D2 runs" is one run with
   three counters, cited by ADR-0012 as the only three-run measurement in the project.
   Both are small edits. Both violate the project's own most explicit rule.
3. **Two subsystems run where the design says they should not.** Recording and the frame
   bus execute on the GUI thread, with a quadratic index rebuild during the operation a
   user deliberately loads the system with. And the camera's own recovery path silently
   wipes every image setting the user has configured.

**What is genuinely at risk if ignored.** Not the demo, and not the next commit — the
**release**. A zip with no licence notice is not redistributable (G-2); a release script
that never runs the tests will happily package a red suite (G-8); a soak that has never
run past 293 seconds cannot support the word "stable" (§34). None of these are hard to
fix, and all of them are the kind of thing that is discovered by a customer rather than
by a test.

**What I would not worry about.** The 8–11 fps at 1280×720 is a measured property of this
sensor at this resolution, confirmed three independent ways, correctly recorded, and
correctly not claimed as a ceiling. The project owner's decision to prefer image quality
over the frame-rate target is legitimate and properly documented. Do not let anyone
"fix" the documentation into claiming 15 fps at HD — that would be the single worst
change available right now.

**The one thing I could not resolve.** Whether `tools/fake_camera.py` — 578 lines that
emulate the real wire format, the real auth scheme and eight fault injections, and which
nothing starts — is a CI fixture that was never wired up or a manual tool that was
never documented. The answer determines whether Task B2 is a two-day job or a two-week
one. That is question INV-1, and it is worth thirty seconds of the author's time before
the backlog is scheduled.

---

## RECOMMENDED NEXT ACTION

**Apply Task A1 first, alone, in its own commit: add `include(CTest)` to
`desktop/CMakeLists.txt` before the `if(BUILD_TESTING)` block (and hoist
`enable_testing()` out of the conditional), then prove it with a fresh configure using
the exact command in `AGENTS.md` — `ctest -N` must report 12 suites from a clean build
directory.**

This is one line, it takes minutes, and it is a prerequisite for everything else: A2,
A3, B1, B2 and B3 all land regression tests, and none of those tests can run — or be
shown to have run — until a clean checkout builds the suite. Every other finding in this
audit is correctly prioritised behind it.

Immediately after A1, in the same working session, apply **Task A2** (the one-line
`medianBytes` correction at `ProfileEngine.cpp:92`, where 14,680 belongs to vga/q24 and
not to the svga/q24 Balanced profile) and add the provenance assertion from A2's
regression test. Two small, independent, high-value corrections that the freshly-working
test suite can then protect.

Do **not** start A3 (the recorder thread move) or C5 (the camera-mutex change) before
those land — both touch hot paths whose safety currently rests on a test suite that a
clean clone does not build.

---
---

# SUPPLEMENT — Firmware Code Audit (added after the first pass)

*The first pass of this audit was written before a dedicated firmware code review
completed. That review surfaced findings my own pass did not reach, because I read the
firmware for architecture conformance rather than for defect density. Every finding
below was independently re-verified against source by me before inclusion. This
supplement is part of the audit; where it conflicts with §1 or §20, this section wins.*

It also surfaced two items that change the *shape* of the backlog, not just its contents:

- **The PSRAM speed the benchmarks were measured at is not the speed the repository
  declares** (FW-3). This is a reproducibility defect in the evidence chain itself, and it
  outranks several P1s below in planning terms.
- **The raw frame transport still contains the exact unbounded-retry bug ADR-0009 was
  written to close** (FW-4). It is compiled off by default, so it is latent rather than
  live — but it is the same defect class, in the same subsystem, three ADRs later.

## S-1. Confirmed defects — firmware

### FW-3 — CONFIRMED DEFECT — P1 — The measured build does not match the committed PSRAM default
- **Files:** `firmware/esp32_cam_stream/sdkconfig.defaults:6` vs `sdkconfig:1066`
- **Evidence (verified):** the committed default declares `CONFIG_SPIRAM_SPEED_80M=y`; the
  actual `sdkconfig` on this machine has `CONFIG_SPIRAM_SPEED_40M=y` and
  `CONFIG_SPIRAM_SPEED=40`. `sdkconfig` is git-ignored (`.gitignore:21`, confirmed with
  `git check-ignore`).
- **Why it matters:** PSRAM speed changes frame-buffer bandwidth, which is plausibly
  relevant to the very thing this project spent Phases 4–6 measuring. Every fps, latency
  and heap figure in ADR-0008/0010/0012 and in all 50 benchmark artifacts was taken on a
  **40 MHz** build while the repository declares **80 MHz**. A clean checkout produces a
  different device than the one measured, so the evidence chain is not reproducible as
  committed — which is the one property §13 and the AGENTS.md measurement rules exist to
  guarantee.
- **Expected:** the committed defaults and the measured configuration must be the same
  configuration, or the difference must be recorded and deliberate.
- **Confidence:** high (both files read directly).

### FW-4 — CONFIRMED DEFECT — P1 — `frame_transport.c` still has ADR-0009's unbounded-retry bug
- **Files:** `firmware/.../frame_transport.c:143-150` (TCP), `:248-252` (UDP)
- **Evidence (verified):** the TCP task allows `++fails > 50` before closing, and each
  failed `esp_camera_fb_get` blocks on the driver's internal timeout. The UDP task has
  **no limit and no recovery** at all. Neither task ever calls `camera_recover()`.
- **Why it matters:** ADR-0009 §Context-3 names this exact failure — a handler tolerating
  50 consecutive capture failures inside a single-threaded task — as the root cause of the
  endless-409 incident. It was fixed in the HTTP handler only
  (`http_servers.c:327-338`). Enabling `CONFIG_SORTING_CAM_FRAME_TRANSPORT` — which the
  Kconfig help text explicitly invites — reintroduces the original bug and holds
  `s_tcp_clients == 1` for minutes while every `/config` call returns 409.
- **Mitigation:** off by default (`Kconfig.projbuild:5`), and no shipped client uses it.
  This is why it is P1 rather than P0.
- **Confidence:** high.

### FW-2 — CONFIRMED DEFECT — P1 — Unauthenticated stack-exhaustion vector in discovery
- **Files:** `firmware/.../discovery.c:27`, `:130-140`
- **Evidence (verified):** `DISCOVERY_STACK` is 4096 bytes; `cJSON_Parse(buf)` runs on a
  received UDP payload on that task with no depth pre-check. cJSON recurses per nesting
  level, and `CJSON_NESTING_LIMIT` in the vendored copy is 1000 — the *buffer* is the only
  bound, not the stack.
- **Why it matters:** a single unauthenticated datagram to UDP 48888, from anyone who has
  joined the softAP, can drive the parser deep enough to trip the FreeRTOS stack-overflow
  canary and reboot the device. That is a remote denial of service on a control surface
  that is otherwise authenticated by design. The same pattern appears on the control
  HTTP path but is behind auth, so discovery is the exposed one.
- **Confidence:** high on the mechanism; medium on the exact nesting depth needed (depends
  on the vendored cJSON build).

### FW-6 — CONFIRMED DEFECT — P1 — NVS restore bypasses the quality floor and XCLK ceiling
- **Files:** `firmware/.../camera.c:331-336` vs `:460-473`
- **Evidence (verified):** restore range-checks `xclk_mhz` 6..27 and `quality <= 63` — a
  *global* range — but never applies `camera_quality_floor(framesize)` or
  `camera_xclk_max_mhz(framesize)`. Both are enforced on the HTTP path only.
- **Why it matters:** ADR-0010 §7 states the firmware "enforces the measured XCLK ceiling
  per resolution so that HD plus a high clock cannot brick the camera." A stored HD@24 MHz
  configuration restores and initialises on boot, bypassing that check — and Phase 5
  measured 24 MHz at HD as producing **no frames at all**. The same gap reopens the
  `FB-OVF` class ADR-0009 closed. Only CRC-valid values are written today, so this is
  defence-in-depth — but restore is precisely where a safety net belongs, and a
  future schema change or a partially-written blob makes it reachable.
- **Confidence:** high on the gap; medium on current reachability.

### FW-1 — CONFIRMED DEFECT — P1 — Recovery discards all persisted sensor controls
This is the same defect as **CP-4** in §6, re-derived independently by the firmware review
and confirmed at `camera.c:498-511` (the `/config` apply path) in addition to
`camera.c:376` (`camera_recover`). Both reinit paths discard user settings; the NVS
persistence ADR-0009 §6 introduced is defeated. **Counted once — see CP-4.**

### FW-8 — CONFIRMED DEFECT — P2 — Watchdog can power-cycle the sensor at 1 Hz
- **Files:** `firmware/.../app_main.c:61-63` vs `:74-75`
- **Evidence (verified):** the stall branch calls `metrics_mark_stream_active()` after a
  successful recovery; the sibling no-capture branch does a bare `continue`. The capture
  clock is only advanced by `metrics_record_capture`.
- **Why it matters:** if the camera re-inits successfully but never delivers a frame, the
  no-capture timer keeps exceeding 30 s and the watchdog calls `camera_recover()` **every
  second, forever**, power-cycling the sensor at 1 Hz. The asymmetry with the adjacent
  branch reads as an omission rather than a decision.
- **Confidence:** high.

### FW-10 — CONFIRMED DEFECT — P2 — Truncated config query is answered 200 OK
- **Files:** `firmware/.../http_servers.c:127`, `:129-134`
- **Evidence:** `httpd_req_get_url_query_str(query[192])` returns
  `ESP_ERR_HTTPD_RESULT_TRUNC` on a long query, so the whole `if` is false and the
  handler falls through returning **200 with the unchanged config**. Likewise a
  `>15`-char value truncates and is silently dropped.
- **Why it matters:** the client cannot distinguish "applied" from "silently ignored".
  A settings change can report success and do nothing.
- **Confidence:** high.

### FW-12 — CONFIRMED DEFECT — P2 — Sensor writes bypass the 409 policy and the camera lock
- **Files:** `firmware/.../http_servers.c:479-526` vs `:135`
- **Evidence (verified):** `sensor_set_handler` has no `stream_client_count()` 409 check
  and no camera mutex, while `config_handler` has both. It also calls `save_nvs` on every
  request (`camera_control.c:282`).
- **Why it matters:** sensor registers can be written mid-capture, concurrently with the
  camera task's DMA. And a slider drag writes NVS on every pixel event — the same flash-wear
  pattern ADR-0009 §5 removed for quality.
- **Confidence:** high on the asymmetry; medium on SCCB-write safety under concurrency.

### FW-13 — CONFIRMED DEFECT — P2 — State-changing endpoint exposed as GET with wildcard CORS
- **Files:** `firmware/.../http_servers.c:647` (registration), `:99,267,290,306,416` (CORS)
- **Evidence (verified):** `/api/v1/config` is registered as **HTTP_GET** and mutates
  camera state. Every response sets `Access-Control-Allow-Origin: *`. No `OPTIONS` handler
  exists, and a simple GET needs no preflight.
- **Why it matters:** any web page the operator's PC visits can issue a cross-origin
  `GET http://192.168.4.1/api/v1/config?framesize=uxga` and read the reply. Auth blocks it
  only because a Bearer token is not auto-attached by a browser — so on a device where
  auth is unprovisioned (see FW-16) this is a full CSRF against camera configuration. A
  state-changing operation should not be reachable by GET.
- **Confidence:** high.

### FW-16 — CONFIRMED DEFECT — P2 — A committed example password can be auto-provisioned
- **Files:** `firmware/.../main/CMakeLists.txt:3-9`, `config_secrets.example.h:6`
- **Evidence (verified):** the example header defines
  `CAMERA_CONTROL_PASSWORD "CHANGE_ME_CONTROL_PASSWORD"` — non-empty, so `provision()`
  succeeds and `s_enabled = true`. (`wifi_start` does reject the placeholder SSID, but
  auth is provisioned independently of the AP coming up.)
- **Why it matters:** a build without real secrets can report `auth.required: true` while
  authenticating against a password published in git. That is fail-open-looking, not
  fail-safe, on the one control surface that §19 requires be authenticated.
- **Confidence:** high on the code path; medium on how easily it occurs in practice.

### FW-17 — CONFIRMED DEFECT — P2 — The documented socket budget is wrong
- **Files:** `firmware/.../Kconfig.projbuild:11-14`, `http_servers.c:624`, `:700`,
  `sdkconfig.defaults:10`
- **Evidence (verified):** the help text budgets "7 + 5 = 12". The source has
  `max_open_sockets = 6` (control) and `2` (stream); each httpd instance consumes
  `max_open_sockets + 3`. Real usage is control 9 + stream 5 + discovery 1 = **15** of
  `CONFIG_LWIP_MAX_SOCKETS=16` — **one free socket**, and the help text omits discovery
  entirely. Enabling the transport adds 3 more, exceeding the pool.
- **Why it matters:** `architecture.md:34-48` makes a strong, correct argument that
  socket exhaustion causes `accept()` ENFILE busy-spin at 100% CPU. That argument is
  correct, and then the arithmetic that follows it is wrong — and the Kconfig text tells
  a future engineer that enabling the transport is affordable when it is not.
- **Confidence:** high (both configs and the default read directly).

### FW-9 — CONFIRMED DEFECT — P2 — The MJPEG stream never sends a closing delimiter
- **Files:** `firmware/.../http_servers.c:298-299`, `:382`
- **Evidence (verified):** the stream sends `--FRAME\r\n` then `\r\n--FRAME\r\n` but never
  the terminating `--FRAME--`. `httpd_resp_send_chunk(req, NULL, 0)` ends the chunked
  transfer, not the multipart body.
- **Why it matters:** RFC 2046 requires a close-delimiter, so the stream is formally
  malformed. `MjpegClient.cpp:14` keys on `"--FRAME"` and is tolerant, which is why it
  works today — but any stricter parser will hang waiting for a boundary that never comes,
  and that is exactly the kind of latent interop failure that surfaces when the desktop
  client is replaced.
- **Confidence:** high.

### FW-11 — CONFIRMED DEFECT — P2 — Frame-size estimate ignores the quality argument
- **Files:** `firmware/.../camera.c:152-160`, `http_servers.c:214-217`, `:251`
- **Evidence:** `camera_estimate_frame_bytes()` contains `(void)quality;` and returns the
  maximum measured **at the floor quality**.
- **Why it matters:** the rejection message for a below-floor quality presents a
  floor-quality figure as though it described the requested quality. At q36 the real frame
  is far larger. This misleads precisely the operator ADR-0009 §2 wanted to inform.
- **Confidence:** high.

### FW-18 — CONFIRMED DEFECT — P2 — The video stream is unauthenticated (disclosed)
- **Files:** `firmware/.../http_servers.c:296-307`; `auth.c:509`
- **Evidence (verified):** `stream_handler` never calls `auth_authorized`. The
  capabilities endpoint honestly advertises `stream_port_protected: false`. Note
  `snapshot_handler` **is** auth-checked (`:275`), so the asymmetry is easy to miss.
- **Why it matters:** this is a **posture decision, not an oversight** — it is disclosed
  in the capabilities output, and §19 explicitly excludes video-stream encryption. I record
  it only because the asymmetry with `snapshot_handler` is undocumented and reads as a bug
  from the status JSON. No change required beyond a written rationale.
- **Confidence:** high that it is as described; **not** a defect in the §19 sense.

### FW-19 / FW-20 — CONFIRMED DEFECT — P2 — Snapshots and mid-frame aborts are uncounted
- **Files:** `firmware/.../http_servers.c:273-294` (snapshot), `:373-376` (send failure),
  `frame_transport.c:163-166`
- **Evidence (verified):** `snapshot_handler` records neither capture nor delivery. On a
  mid-frame send error the handler breaks out logging only — no counter. `near_budget_frames`
  is logged but never exported.
- **Why it matters:** AGENTS.md states "every drop/corruption must be counted, never
  hidden." A client aborting mid-frame loses data with no exported evidence. The snapshot
  omission is defensible (a snapshot should not satisfy the liveness watchdog) but is
  undocumented and easy to misread from the status JSON.
- **Confidence:** high.

### Remaining firmware findings, individually lower severity
| ID | Sev | Finding |
|---|---|---|
| FW-14 | P2 | `to_hex(slot->token, …, token_out)` writes 33 bytes then `(void)token_len;` — the API accepts a buffer size and ignores it. No overflow today (single caller passes `char[64]`), but the signature invites one. |
| FW-21 | P3 | `save_nvs` on every successful sensor apply, outside any lock: NVS write per slider event, plus ms-scale blocking in the single-threaded control httpd task. |
| FW-22 | P3 | `hdr_len` is the `snprintf` return used unchecked as the send length for `part_hdr[128]`. Currently maxes ~93 B, so no truncation — one format-string edit from an OOB read. |
| FW-23 | P3 | No PSRAM check before `fb_location = CAMERA_FB_IN_PSRAM`. On camera failure: 3 retries, 5 s delay, `esp_restart()` — an unbounded reboot loop with no HTTP surface to diagnose through. Fail-fast, but the signature is a silent boot loop. |
| FW-24 | P3 | `client_slot(ip, true)` evicts the entry with the **fewest** fails — normally the legitimate client's `fails == 0` entry — clearing its backoff and lockout. Eviction policy is backwards. |
| FW-25 | P3 | `ssid_len = strlen(ssid)` after `strncpy(…, 31)`; password checked for `< 8` but not `> 63`. An over-long SSID yields a truncated buffer with a length field describing the untruncated string; an over-long password is silently truncated. |
| FW-26 | P3 | Discovery reply rate limit is a single global `last_reply_us`, not per-source: one client can suppress discovery for every other client indefinitely. |
| FW-27 | P3 | `atoi()` on caller-supplied strings in three places. Overflow is UB; `strtol` with range checking would be correct. Impact is low (a wrapping value still lands in-range or is rejected) but the pattern has no error channel. |

### Documentation nit the firmware review caught, worth recording
ADR-0009 §1 says "256,144-byte budget (256 KiB)" — internally inconsistent. The code and
`sdkconfig.defaults:13` use **262144**, which *is* 256 KiB. **The code is right; fix the
ADR** so a future agent does not "correct" the firmware to match the typo. (Supersedes
the socket-count part of DD-4 in §12, which this review refines: the real figure is 15 of
16, not 12 of 16.)

## S-2. One desktop finding the first pass did not reach

### DX-1 — CONFIRMED DEFECT — P1 — "Sign-in required" is rendered as a permanent red error
- **Files:** `desktop/src/SessionState.cpp:84-88`, `CameraDevice.cpp:44-48`,
  `NotificationCenter.cpp:53-57`
- **Evidence (verified):** `SessionState::derive` sets `out.state = Error` with
  `out.severity = "warn"` for `needsSignIn` — correctly treating it as actionable rather
  than a fault. But `CameraDevice` dispatches on `state() == State::Error` and **ignores
  severity**, posting `Diagnostics::Level::Error`. `NotificationCenter::isSticky` treats
  `>= Warning` as sticky, so it never auto-expires.
- **Why it matters:** an ordinary first-run state — "the camera requires a sign-in" —
  becomes a permanent red error card that the user must dismiss by hand, on every
  connect, before they have done anything wrong. The severity plumbing exists and works
  at the state-machine layer; the consumer of that layer discards it.
- **Confidence:** high.

## S-3. What the firmware review confirmed as correct

Recorded so the implementation agent does not re-audit it, and because it is the stronger
half of the story:

- **The mutex/lifetime invariant holds.** `s_cam_mutex` is held from `camera_fb_get` to
  `camera_fb_return`, so `esp_camera_deinit()` cannot run with a frame in flight. No path
  returns a frame twice; all four `fb_get` sites NULL-check. (FW-7 is about lock *scope*
  across I/O, not this invariant.)
- **Recovery bound matches ADR-0009 §4**: one failed capture plus two recovery attempts.
- **Quality floors and the XCLK ceiling are enforced on the HTTP path**, with 400s that
  name the floor and the resolution, and 409 while a stream client is attached.
- **Quality-only changes skip reinit** via `sensor->set_quality` — ADR-0009 §5 satisfied.
  Config apply rolls back to the prior config on failure and correctly does not re-save NVS.
- **Auth crypto is real.** PBKDF2-HMAC-SHA256 via `mbedtls_pkcs5_pbkdf2_hmac_ext`; proof is
  HMAC-SHA256(verifier, nonce); salt+verifier in NVS with **the password never stored**; all
  comparisons timing-safe via `ct_equal`; nonce single-use (zeroed on both success and
  failure) with a 30 s TTL; 128-bit token from `esp_fill_random`; exponential backoff plus
  5-minute lockout at 8 failures. `from_hex` is always preceded by an exact `strlen` check,
  so no OOB read. No secret in any log; `config_secrets.h` is git-ignored.
- **Discovery reply addressing is correct** — derived from `recvfrom`'s `peer` including
  source port, bound `INADDR_ANY`, so it works in any topology.
- **TCP framing matches `protocol.md` byte-for-byte** — 36-byte packed struct with
  `_Static_assert`, CRC32 over payload only, sequence wrap, device id from MAC.
- **All `snprintf` calls are size-bounded**; `body[513]`/`body[257]` receive at most
  512/256 bytes then NUL. No off-by-one.
- **Watchdogs cannot fire while idle** — both gated on `clients > 0`, and the
  `metrics_us_since_*` helpers return 0 before the first event.
- **The DRAM feasibility pre-check** implements ADR-0008 §4, rejecting infeasible requests
  before touching the camera.
- **No station-mode code anywhere**; `wifi.c` is AP-only and matches ADR-0006 exactly.
- **`FrameImageProvider`'s resolver reading the active bus per request is correct** — this
  is what lets the active camera change between two frames without the view going stale.

## S-4. Amendments to the backlog in §17

| Change | Rationale |
|---|---|
| **New Task A0 — reconcile PSRAM speed (FW-3)** | Outranks everything except A1. Until the measured configuration is reproducible from a clean checkout, no future benchmark is comparable to the existing record, and the ≥1 h soak would be measured on a third configuration. Two sub-steps: decide 40 or 80 MHz, make `sdkconfig.defaults` match, and re-measure one HD/q12 point to confirm the choice does not move the frame rate. |
| **New Task A5 — fix the firmware's unauthenticated discovery DoS (FW-2) and the CSRF shape (FW-13)** | Both are network-facing and both are cheap: bound the JSON depth before parsing (or parse with a depth-limited variant), and stop exposing a mutating operation as GET. |
| **New Task C8 — apply the safety net on NVS restore (FW-6)** | Pairs with the existing C4 (enum validation) as one change to `camera_cfg_restore()`. |
| **New Task C9 — fix the watchdog 1 Hz loop (FW-8)** | Small, self-contained, and the asymmetry with the adjacent branch makes it an obvious omission. |
| **New Task C10 — correct the socket budget arithmetic (FW-17)** | Documentation + Kconfig text only, but it currently understates usage by 3 sockets and omits discovery. Do this before anyone enables the transport. |
| **New Task B7 — separate the session-error severity (DX-1)** | One-line fix in `CameraDevice`'s dispatch: respect `SessionState::severity` instead of mapping every `Error` state to a red notification. |
| **New Task C11 — `frame_transport` bounded recovery (FW-4)** | Low priority *because it is compiled off*, but it must be fixed before `CONFIG_SORTING_CAM_FRAME_TRANSPORT` is ever enabled, and the Kconfig help text currently invites that. |

**Re-prioritised recommended next action.** Unchanged in substance, with one addition:
A1 first (as stated under **RECOMMENDED NEXT ACTION** above), then **A0 (PSRAM speed)**
before A2, because A0 determines whether the existing benchmark record means anything for
a clean build. A2, A3, B1–B3 follow as originally ordered.
