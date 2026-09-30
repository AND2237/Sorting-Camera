# Remediation tracker — Phase 6 audit (2026-09-29)

Working record for closing `GEMINI_PHASE6_AUDIT_2026-09-29.md`. Every row carries the
evidence that closed it: a command and its output, a test name, or a measurement.

The audit file itself is a frozen record and is never modified.

Status key: `open` → `fixed` (code changed, build green) → `verified` (regression test or
measurement proves the fix) → `closed` (gate evidence complete). `deferred` means the
finding was triaged and consciously **not** done in this remediation, with its owner
named on the row (Phase 8, or the audit's own optional group) — never silently dropped.

---

## Stage 0 — baseline (2026-09-29)

Recorded before any source edit, from a clean tree at `f3258d5`.

| Check | Command | Result |
|---|---|---|
| Desktop configure (documented) | `cmake -S desktop -B desktop\build-baseline -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\mingw_64` | exit 0 |
| Test targets registered | `ninja -C desktop\build-baseline -t targets all \| findstr tst_` | **0** |
| CTest suites | `ctest --test-dir desktop\build-baseline -N` | **Total Tests: 0** |
| `CTestTestfile.cmake` present | `Test-Path desktop\build-baseline\CTestTestfile.cmake` | **False** |
| `BUILD_TESTING` in cache | `Select-String CMakeCache.txt BUILD_TESTING` | **absent** (never defined) |
| Desktop build | `cmake --build desktop\build-baseline` | exit 0, 47/47, `SortingCamera.exe` |
| Firmware clean configure | copy to scratch without `sdkconfig`, `idf.py set-target esp32` | exit 0 |
| Firmware clean build config | generated `sdkconfig` | `ESPTOOLPY_FLASHFREQ="40m"`, `CONFIG_SPIRAM_SPEED_40M=y` |
| Firmware measured config | `firmware\esp32_cam_stream\sdkconfig` (gitignored, as measured) | `ESPTOOLPY_FLASHFREQ="40m"`, `CONFIG_SPIRAM_SPEED_40M=y` |
| ESP-IDF diagnostic on dropped line | delete `sdkconfig`, `idf.py reconfigure`, grep output | **no warning emitted** |

### Finding: the audit's FW-3 divergence does not exist

The audit states the measured build and the committed configuration diverge on PSRAM speed.
They do not. A clean checkout configures to **exactly** the configuration the benchmarks were
measured on.

Mechanism, established empirically rather than by reading:

- `sdkconfig.defaults` sets `CONFIG_SPIRAM_SPEED_80M=y`.
- In `esp_psram/esp32/Kconfig.spiram`, `SPIRAM_SPEED_80M` carries
  `depends on ESPTOOLPY_FLASHFREQ_80M`.
- `sdkconfig.defaults` never sets flash frequency, and `spi_flash/esp32/Kconfig.flash_freq`
  defaults it to `40M`. The dependency is therefore unmet.
- ESP-IDF **silently discards** the `80M` value. No warning, no diagnostic.
- Result: clean configure → `40M`; measured build → `40M`. Identical.

Consequence: **no historical figure is invalidated and no re-baseline is required for
correctness.** The defect is a misleading line that claims a configuration the build never
used. The audit's conclusion ("retains the old configuration") happens to be correct, but
the reasoning and severity are wrong.

Tracked under **FW-3** below. Decision recorded in `docs/decisions/0015-psram-speed-default.md`.

### Note: `esp32-camera` has an available update

Clean configure reports `espressif/esp32-camera` `2.1.7` → `2.1.8`. **Do not update.** The
lock file is pinned deliberately (ADR-0005) and updating would change the configuration under
a frozen measurement record.

### Note: tree is not clean at baseline

`docs/audits/GEMINI_PHASE6_AUDIT_2026-09-29.md` is staged but uncommitted. The audit's §3
claim of a clean working tree is incorrect. The file is left exactly as found.

---

## Stage 1 — A1 (CP-1): test registration (2026-09-29)

Root cause: `desktop/CMakeLists.txt` guarded on `BUILD_TESTING` but nothing ever called
`include(CTest)`. That module is what defines the `BUILD_TESTING` option and calls
`enable_testing()`. Without it the variable was undefined, CMake read it as false, the
`add_subdirectory` was skipped, and the build reported success while running nothing.
`enable_testing()` was also inside the same guard, so it was unreachable regardless.

Fix: `include(CTest)` called unconditionally before the guard. The redundant conditional
`enable_testing()` is removed — `CTest.cmake:63,84` already performs it when testing is on.

Gate, from the exact documented command in a **fresh** `desktop\build` (the old directory
was deleted first, because it carried a hand-typed `BUILD_TESTING:UNINITIALIZED=ON`):

| Check | Result |
|---|---|
| `BUILD_TESTING` in cache | `BOOL=ON` (now defined by CTest) |
| `CTestTestfile.cmake` (root / `tests/`) | `True` / `True` |
| `ctest -N` | **Total Tests: 12** |
| build | exit 0, 95 targets (47 → 95 with the suites) |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 16.49 s, exit 0 |

All twelve suites pass on first execution, so no latent breakage was hidden by the gap.

---

## Stage 2 — A0 / FW-3: PSRAM speed (2026-09-29)

The audit's premise does not hold. `CONFIG_SPIRAM_SPEED_80M=y` in `sdkconfig.defaults`
**never took effect**: `SPIRAM_SPEED_80M depends on ESPTOOLPY_FLASHFREQ_80M`
(`esp_psram/esp32/Kconfig.spiram`), flash frequency is never set there and defaults to
`40M`, so `kconfgen` discards the value — with **no warning whatsoever**, which is what made
the line convincing for as long as it did.

Therefore:

- A clean checkout produces `CONFIG_SPIRAM_SPEED_40M=y`.
- The measured build ran `CONFIG_SPIRAM_SPEED_40M=y`.
- **They already matched.** There was nothing to reconcile.

Fix: state `40M` explicitly and comment the dependency hazard at the site. Decision and full
evidence in `docs/decisions/0015-psram-speed-default.md`.

**Verification (stronger than the re-measurement the audit proposed):** regenerating the
configuration from a clean checkout before and after the change gives a **byte-identical**
72,127-byte `sdkconfig`. The fix provably perturbs no configuration value, so a hardware
re-baseline would be measuring a configuration that did not change. The audit's A0
sub-step *"re-measure one HD/q12 point to confirm the choice does not move the frame rate"*
is discharged by the diff instead, and costs no hardware time.

No artifact in `benchmarks/results/` records a clock frequency (`free_spiram` is capacity
telemetry), so nothing in the record is invalidated, relabelled or discarded.

---

## Stage 3 — A2 (CP-2, CP-16, CP-17, CP-25): profile ladder provenance (2026-09-29)

Four findings, all in the operating-profile ladder, all about numbers published to the user.

### CP-2 — Balanced carried another profile's byte count

`p.medianBytes = 14680.0` on the svga/q24 profile. That value occurs in exactly one cell of
the Phase 4 artifact — **vga/q24, fb2, confirm**. The profile's own configuration measured
**16,982 B** (envelope, fb3) or 23,077 B (confirm, fb2). Corrected to the envelope cell, the
same cell `measuredFps` came from. The comment claiming the confirm stage "did not test this
point" was also false: it did, at 14.37 fps / 23,077 B, and `docs/benchmark-results.md`
records that cell under *measured, excluded from ladder*, with `ADR-0008` decision 3 listing
it under *excluded by measurement*. `altMeasuredFps` stays `0.0` per the owner's instruction —
the field now means "no second reading was **admitted**", and the comment and evidence say so.

### CP-16 — provisional floor presented as settled, contradicted by its own doc

`benchmark-results.md:447-457` was headed *"HD/q12 is an experimental profile, not the
production default"* and said *"The production default is not yet chosen"* — written
2026-09-27, superseded the same day by ADR-0010 decision 1. Rewritten to record the owner's
choice, keep the 7.7–8.4 fps measurement and the unmet §34 target visible (ADR-0010 decision
3), and state that the ≥7 fps floor is provisional pending the ≥1 h D1 soak.

On the code side, `Profile` gained `floorProvisional` (set only on `High Quality`), the
evidence string now says *"7 fps floor is provisional per ADR-0010 pending the 1 h soak"*, and
a new `activeFloorProvisional` Q_PROPERTY gives the UI a flag to use rather than rendering
7.0 as a settled target.

### CP-17 — one run described as three

`altMeasuredFps = 9.98; // mean of the three D2 runs` — the artifact is **one** 120 s run;
`capture_fps` 10.000, `delivery_fps` 9.966 and `decoded_fps` 9.958 are three measures of it,
mean 9.9748 → **9.97**, not 9.98. The evidence string's *"58 KB"* was from the 2026-09-29
recheck runs; the D2 artifact's own mean is **28,956 B**. Both corrected, and the string now
says *"one run measured three ways, not three runs"*. `ADR-0012`'s *"the only ladder point
with a three-run measurement"* corrected the same way.

### CP-25 — the documented conservative rule was never implemented

`recommended()` tested `p.measuredFps >= p.floorFps` and never consulted `altMeasuredFps`,
contrary to its own comment. `ProfileEngine::conservativeFps()` now implements the rule and
`recommendedIn()`/`recommended()` use it. `activeMeetsFloor()` — what `Main.qml:1318` colours
red with and `CameraDevice.cpp:148` prints as `UNDER FLOOR` — was reading the optimistic
envelope only, which is the user-visible half of the same finding, so it now reads the
conservative figure too.

### Provenance tests

`tests/CMakeLists.txt` now defines `SCAM_BENCH_DIR`, and `tst_profileengine` gained six slots
that resolve every citation in `buildLadder()` back against `benchmarks/results/` at run time:

| Test | Guards |
|---|---|
| `everyPublishedFigureExistsInTheArtifactItCites` | each cited fps/bytes is really in the file it names |
| `noPublishedFigureComesFromAnotherConfiguration` | the cited cell's resolution and quality are the profile's own; pins 16,982 and rejects 14,680 |
| `everyLadderFigureIsCoveredByACitation` | `measuredFps`, `altMeasuredFps`, `medianBytes` each appear in a citation |
| `evidenceQuotesTheFiguresItCites` | the user-facing evidence string quotes them at published precision |
| `theConservativeReadingDrivesTheAutomaticChoice` | the rule; forces the optimistic and conservative readings to disagree |
| `onlyARecordedDecisionCarriesAReducedFloor` | any floor below 15 must be marked provisional, and only HD's is |

Equality is on the **published** precision — two decimals for fps, whole bytes for sizes —
not a `0.01` tolerance. That distinction was found by mutation: with a `<= 0.01` tolerance,
`altMeasuredFps = 9.98` against a citation of `9.97` passed. It now fails.

**Mutation evidence** (introduce the defect, rebuild that target, expect non-zero exit,
revert):

| Mutation | Result |
|---|---|
| Balanced `medianBytes` 16982 → 14680 | exit 2, two failures (`noPublishedFigure…`, `everyLadderFigureIsCovered…`) |
| drop `floorProvisional = true` | exit 1, `onlyARecordedDecisionCarriesAReducedFloor` |
| `altMeasuredFps` 9.97 → 9.98 | exit 1, `everyLadderFigureIsCoveredByACitation` |

### Documentation

- `docs/benchmark-results.md` — added the p50-byte column for all 28 envelope cells, added
  byte figures to the four below-floor confirm cells, replaced the stale
  *"experimental profile, not the production default"* section. Verified programmatically:
  **all 31 published byte figures and all 29 published fps figures now occur in the file**,
  which is what `ADR-0012` decision 1 requires of every ladder number.
- `docs/decisions/0012-operating-profile-ladder.md` — corrected three false statements:
  the confirm run is **Phase 4**, not Phase 5; it *did* cover svga/q24 (decision 3); and
  the D2 artifact is one run measured three ways, not "a three-run measurement".
- `docs/decisions/0010-…` — unchanged. It already recorded the owner's choice and the
  provisional floor correctly; the benchmark document was the stale side of the conflict.

### Gate

| Check | Result |
|---|---|
| build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 16.40 s, exit 0 |
| `tst_profileengine` | **Totals: 23 passed, 0 failed** |

---

## Stage 4 — remaining findings (2026-09-29)

### g1 — CP-3: recording and the frame bus ran on the GUI thread

`MjpegClient` moved only its worker onto the network thread. The worker→client hop used the
default connection type, which queued to `MjpegClient`'s GUI affinity, so `frameReady` was
*emitted* on the GUI thread and both downstream consumers ran there too: `CameraDevice.cpp:170`
writing `FrameBus` and feeding `Recorder`. That is the reverse of the thread table at
`docs/architecture.md:124-128`, which puts the transport, the bus and recording on the network
thread, and it is what the GUI-thread rule at `architecture.md:134` exists to prevent.

Three changes:

1. **`MjpegClient.cpp`** — the worker→client hop is now `Qt::DirectConnection`, so the lambda
   and the `emit frameReady` run on the network thread. The per-frame GUI property bookkeeping
   that used to run before the emit (`setRetryAttempt`, `setConnecting`, `setNoResponseStreak`,
   `setReconnecting`, `setRecoveryHint`) is queued back to the owning thread with
   `QMetaObject::invokeMethod(…, Qt::QueuedConnection)`, so no QML property is written from
   off-thread. `AppMetrics::addPresentAgeMs` is already mutex-guarded.
2. **`CameraDevice.cpp:170`** — the frame-delivery connection is now `Qt::DirectConnection`, so
   `FrameBus::setFrame` and `Recorder::appendFrame` are reached on the network thread. The
   connection at `CameraDevice.cpp:99` (Diagnostics frame counters) is deliberately left Auto
   and therefore becomes queued to the GUI thread, where `Diagnostics` lives.
3. **`Recorder`** is now internally synchronised. `start()`, `stop()`, `appendFrame()` and every
   `Q_PROPERTY` reader take `m_mutex`; `writeIndex()` takes `m_indexMutex` first and `m_mutex`
   second, and that nesting is never reversed anywhere else, so the pair cannot deadlock.
   Taking them in that order is what makes the last sidecar written also the most complete:
   `stop()` closes the file under `m_mutex` before it indexes, so an in-flight periodic flush
   cannot leave the sidecar describing fewer frames than the container. Signals are emitted only
   after both locks are released, because a slot is free to read a property back and would
   otherwise re-enter the lock it is being emitted under. The O(N) sidecar serialisation runs
   outside `m_mutex`, so the frame path is never blocked behind disk I/O.

No change to the `.scamrec` or sidecar format (ADR-0011): `tst_recorder`'s existing sidecar and
byte-identity assertions pass unmodified.

### Tests

| Test | Guards |
|---|---|
| `TestCapture::framesAreDeliveredOffTheGuiThread` | `frameReady` is not emitted on the GUI thread — the CP-3 defect itself |
| `TestRecorder::stopRacingTheFramePathKeepsContainerAndIndexInAgreement` | four threads feed one `Recorder` while this thread reads the QML properties and stops it; the container is then contiguous and duplicate-free and the sidecar describes exactly the frames it holds |

The second test does **not** prove the absence of data races. That needs a thread sanitizer,
which the MinGW toolchain does not provide, and removing every `m_mutex` lock from
`Recorder.cpp` left it green over 20 runs. Its stated job is narrower: a lock-order mistake
between `m_mutex` and `m_indexMutex` shows up as the 30 s deadline being reached rather than a
hang, and a `writeIndex()` that snapshots outside the serialising lock shows up as a sidecar
that disagrees with the container. The locking itself is established by construction in
`Recorder.h`, not by this test.

**Mutation evidence** (introduce the defect, rebuild, expect non-zero exit, revert):

| Mutation | Result |
|---|---|
| `MjpegClient.cpp` worker→client hop `Qt::DirectConnection` → default Auto | exit 1, `framesAreDeliveredOffTheGuiThread`, 15/15 runs; reverted, 6/6 green |
| every `m_mutex` lock in `Recorder.cpp` → uncontended mutex | exit 0, 20/20 — **not caught**, see above |

### g1 (part 2) — CP-19 / FW-7: the camera lock was held across the network send

Firmware. Full write-up: `docs/decisions/0016-camera-frame-buffer-drain-gate.md`.

**What was wrong.** `camera_fb_get()` took `s_cam_mutex` and did not release it;
`camera_fb_return()` released it, so a frame buffer's whole lifetime pinned the
camera lock. Four sites therefore held it across network I/O: `snapshot_handler`
(`http_servers.c:278-292`), `stream_handler` (`http_servers.c:323-371`),
`tcp_task` (`frame_transport.c:141-162`), `udp_task` (`frame_transport.c:247-288`).
Everything else queued behind one client's socket — including `camera_recover()`,
the path that exists to un-wedge the camera.

**What changed** (`firmware/esp32_cam_stream/main/camera.c`):

- Added `s_gate_closed`, `s_gets_in_flight`, `s_fb_held` (all under `s_cam_mutex`),
  `camera_gate_close()` / `camera_gate_open()`, `CAM_TEARDOWN_TIMEOUT_MS` 7000,
  `CAM_GATE_POLL_MS` 10.
- `camera_gate_close()` takes the mutex, sets the gate, and **releases the mutex on
  every poll iteration** until both counters are 0 — so consumers are never queued
  behind the teardown and the teardown is never queued behind them.
- `camera_fb_get()` releases the mutex around the driver call (which blocks up to
  `FB_GET_TIMEOUT` = 4000 ms) under `s_gets_in_flight`. If the gate closed while it
  was inside, it gives the frame back and loops instead of returning NULL —
  because `STREAM_CAPTURE_FAIL_LIMIT` is 1, a NULL here would send the stream
  handler into recovery and then close the client.
- `camera_fb_return()` takes the lock it previously assumed, and decrements
  `s_fb_held`.
- `camera_recover()` and `camera_apply_config()` close the gate before touching the
  driver and open it on **every** path out (success, rollback, and the close-failure
  path leaves it untouched and returns `ESP_ERR_INVALID_STATE`).
- Removed `s_recovering`: redundant under the gate, and harmful — an early return
  during another task's drain-poll would make `app_main.c:66,78` `esp_restart()`.

**Checked against source, not assumed:**

| Claim | Source |
|---|---|
| the lock is held across a send at 4 sites | `http_servers.c:278-292,323-371`; `frame_transport.c:141-162,247-288` |
| `esp_camera_fb_get()` blocks inside the driver | vendored `esp_camera.c:387-389`, `FB_GET_TIMEOUT` = 4000 ms |
| `esp_camera_deinit()` frees the buffers | vendored `esp_camera.c:373` → `cam_hal.c:635-673` |
| `esp_camera_fb_return()` becomes unsafe after deinit | `esp_camera.c:404` early-returns on NULL `s_state`, set *after* the free |
| 5 s send timeout on HTTP | `esp_http_server.h:68` `send_wait_timeout = 5`, applied at `httpd_main.c:91` |
| 5 s send timeout on TCP/UDP | `frame_transport.c:131-132`, `TX_TIMEOUT_S` `:23` |

**Gate (build + desktop regression)**

| Check | Result |
|---|---|
| `idf.py build` (ESP-IDF v5.5.4) | exit 0, no warnings in `camera.c`, `esp32_cam_stream.bin` 0xf7710 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12** |

**Status `fixed`, not `closed`.** There is no firmware test harness, so the only
evidence available off-hardware is a clean build and an unchanged desktop suite.
The behaviour has **not** been observed on the camera: flash and live validation is
Stage 4 g5. Do not close this row before that.

**Not addressed by CP-19 (observation):** the `only_quality` fast path in
`camera_apply_config()` calls `sensor->set_quality()` with no lock and no gate — a
pre-existing race against a concurrent teardown, unchanged by this work.

### Still open in g1

None — g1 (CP-3 and CP-19 / FW-7) is code-complete.

### Gate

| Check | Result |
|---|---|
| build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 16.57 s, exit 0 |
| repeat `ctest` | 4/4 rounds green |
| `tst_recorder` | **Totals: 10 passed, 0 failed** |
| `tst_capture` | **Totals: 9 passed, 0 failed, 1 skipped** (live test, needs `SCAM_TEST_HOST`) |

### g2 — CP-6: a 401 on a config write silently dropped the change

Desktop. `desktop/src/DeviceStatus.cpp`, `tests/tst_devicestatus.cpp`.

**What was wrong.** `trySendPendingConfig()` clears `m_pendingKv` before the
request is sent (`DeviceStatus.cpp:1061` today, `:924` in the audited snapshot),
so the batch survives only as
`m_cfgQuery` inside the worker. On a 401 the handler called `m_auth->signIn()`
and emitted `configFinished(false, "session expired, signing in again")` —
wording that promises a retry — and queued nothing. The write was gone while the
UI showed a transient message. The audit notes the same path on `setSensor`.

**What changed:**

- The worker keeps `m_cfgAuthRetryPending` / `m_sensorAuthRetryPending` and the
  body of the rejected write. While either is set, no `configFinished` /
  `sensorFinished` is emitted, so `m_configInFlight` and `m_sensorBusy` stay
  true and the panel stays busy **on purpose** rather than reporting a failure
  that has not been decided yet.
- `AuthClient::settled()` — emitted on every terminal outcome of a sign-in — now
  calls `resumeWritesAfterSignIn()`: authenticated → replay the stored write
  exactly as it was built; not → one honest failure carrying
  `AuthClient::lastError()`.
- `armAuthRetry()` arms a single-shot `kAuthRetryWaitMs` (15 s) bound, because
  `AuthClient::invalidate()` aborts an in-flight sign-in **without** emitting
  `settled()`. Without the bound, a device switch or `forgetCredential()` in the
  retry window would leave the panel busy for ever.
- `signIn()` declining silently (the re-auth cooldown at `AuthClient.cpp:196`)
  is caught by the `!isBusy()` check immediately after the call.
- One replay per write: a second 401 fails with "the camera rejected the write
  twice" instead of looping.
- `setSensor` was split into `setSensor` (store) and `sendSensor` (send) so the
  sensor write can be replayed by the same mechanism.

**Tests**

| Slot | What it pins |
|---|---|
| `aConfigWriteRejectedWith401IsReplayedAfterSignIn` | a rejected batch leaves the machine **twice**, the replay still carries `framesize=hd` *and* `quality=12` together, one fresh sign-in happens, `configError()` ends empty |
| `aConfigWriteRejectedWith401FailsHonestlyWhenSignInCannotStart` | with no credential the write fails **once** with a message containing `session expired` and **not** containing `signing in again`, the panel is released, and no extra request is sent |

The stub gained `/api/v1/auth/challenge`, `/api/v1/auth/login`, a
`configUnauthorized` flag that makes `/api/v1/config` answer 401 until a login
succeeds, and counters for both.

**Mutation** — reverting only the 401 branch of `doConfig()` to its original
three lines:

| Check | Result |
|---|---|
| build | exit 0 |
| `tst_devicestatus` | **exit 2, 11 passed, 2 failed** — `aConfigWrite…ReplayedAfterSignIn` fails on `configError() == "session expired, signing in again"`; `aConfigWrite…FailsHonestly…` fails on "the message promised a retry that will not happen" |
| restored | exit 0, 13/13 |

**Gate**

| Check | Result |
|---|---|
| build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12** |
| repeat `ctest` | 4/4 rounds green |

---

### g3 — CP-5, CP-18/FW-6, CP-4/FW-1

Three findings, two areas.

#### CP-5 (desktop): the recovery budget never re-armed

`desktop/src/MjpegClient.cpp`, `tests/tst_capture.cpp`.

**What was wrong.** `m_recoveriesTriggered` counts up against `kMaxRetries`
with no reset anywhere in `frameReady` / `start()` / `stop()`, while
`setRecoveryHint(QString())` already cleared the hint at all three. The counter
and the hint therefore disagreed about whether the camera had been recovered
from: after two `deviceRecoveryRequested()` emissions the device was never
asked to re-initialise again for the life of the process. The audit's
trigger — a camera that ran cleanly for an hour and then wedged — gets no
self-repair at all.

**What changed:** `m_recoveriesTriggered = 0;` added at exactly the three sites
where `setRecoveryHint(QString())` already resets, so budget and hint move
together.

**Test** — `recoveryBudgetRearmsAfterTheStreamIsHealthyAgain` drives a
**three-episode** cycle: two `no response from camera` failures (the only
error feeding the budget) → recovery; then the stub answers so frames arrive
and the streak is asserted back to `0`; then a `missing or invalid
Content-Length` failure, which is deliberately *not* a first-byte timeout so it
restarting the streak does the re-arming; repeat to a third episode.
`MjpegStub` gained `silent` (accept the connection, answer nothing → drives the
6 s first-byte watchdog) and `sendBrokenPart()` (writes a non-numeric
`Content-Length`). Three episodes are required because the budget is 2 — a
two-episode test passes under the defect.

**Mutation** — removing only the `frameReady` lambda's reset, leaving
`start()`/`stop()`:

| Check | Result |
|---|---|
| build | exit 0 |
| `tst_capture` (2 episodes) | **exit 0 — not caught** (why the test was rewritten to 3) |
| `tst_capture` (3 episodes) | **exit 1, 9 passed, 1 failed, 1 skipped**, 177583 ms — `recoveries == episode` returned FALSE on episode 3 |
| restored | exit 0, 10 passed, 0 failed, 1 skipped, 47478 ms |

#### CP-18 / FW-6 (firmware): NVS restore trusted the bytes

`firmware/esp32_cam_stream/main/camera.c` → `camera_cfg_restore()`.

**What was wrong.** `grab_mode` and `fb_location` are enum tags cast straight
out of the record with no range check at all (`rec.grab_mode` passes the
`> 63`/`< 6` style guards because those test other fields), and the stored
`quality` / `xclk_mhz` were compared only against their absolute bounds —
never against the *measured* per-resolution ceilings that
`camera_quality_floor()` and `camera_xclk_max_mhz()` enforce on the HTTP path
(`camera_apply_config`, `camera.c:567` / `:575`). Restore was the door around
ADR-0009: a stored HD@24 MHz, which Phase 5 measured as producing **no frames
at all**, came back to life on every boot.

**What changed:**

- enum validation for both fields before they are cast — unknown tag →
  "using defaults", the same behaviour as the existing range guards.
- `camera_quality_floor(stored_fs)` and `camera_xclk_max_mhz(stored_fs)` are
  applied on restore, not duplicated as literals — the numbers change when
  Phase 5 re-measures and the restore path must follow.
- **Reject, not clamp**, matching what `camera_apply_config` does with the
  same inputs: the operating point is refused rather than silently replaced by
  a different one.

#### CP-4 / FW-1 (firmware): controls lost on every re-init

`firmware/esp32_cam_stream/main/camera.c`, `main/app_main.c`.

**What was wrong.** `camera_driver_init()` wrote nine sensor controls to
hard-coded values on every successful `esp_camera_init()`, and
`camera_control_init()` — the thing that reads NVS and restores what the user
chose — ran exactly once, at `app_main.c:113`. Every later path that re-inits
the driver (the stall watchdog's `camera_recover()`, any framesize / xclk /
fb_count change through `camera_apply_config`) therefore handed the viewer back
brightness 0 and contrast 0 with no log line to explain the picture.

**What changed.** The restore moved *into* `camera_driver_init()`, after the
baseline defaults and only on success, so all three callers — boot, recovery,
config apply — end at the same place. The now-duplicate call at
`app_main.c:113` was removed; it was reachable only after a successful
`camera_init()` (the failure path `esp_restart()`s), so boot behaviour is
unchanged. Both control sets are NVS/SCCB work already performed under
`s_cam_mutex` in `camera_apply_config`, so doing it inside the drain gate adds
no new lock scope.

**Firmware gate** — no host-side harness exists for these three (CP-18/FW-6,
CP-4/FW-1), so they are **build-verified only** and are recorded as `fixed`,
not `closed`. Behaviour must be confirmed on hardware in Stage 4 g5:

| Check | Result |
|---|---|
| `idf.py build` | exit 0, no `camera.c` warnings, no `app_main.c` warnings |

#### Gate

| Check | Result |
|---|---|
| `idf.py build` | exit 0 |
| desktop build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 62.15 s |
| CP-5 mutation | caught (exit 1) |
| CP-18/FW-6, CP-4/FW-1 | build-verified; hardware validation **not run** (Stage 4 g5) |

---

### g4 — CP-10, CP-14, CP-15, CP-23, DX-1: the QML presentation defects

Five findings, four of them in `desktop/qml/Main.qml`. Only CP-15 and DX-1 have
a C++ surface that a test can reach; the other three are QML-only and are held
by `qmllint` and a runtime load instead (see the gate).

#### CP-10 — three status overlays at identical coordinates

`Main.qml` (the three bottom-left labels).

**What was wrong.** The reconnect counter, the camera-recovery counter and the
stream error were each anchored `left`/`bottom` with `anchors.margins: 14` and
no offset between them. The conditions are provably co-satisfiable —
`camera_recoveries` is cumulative for the boot, so after any recovery a later
stream error printed *underneath* the recovery counter and which one an
operator could read was pure z-order.

**What changed.** The three are wrapped in a `Column` on the same anchors.
`Column` skips children whose `visible` is false, so exactly the conditions that
hold are stacked and the offset from the corner is the same 14 px.

#### CP-14 — sensor sliders lose their status binding on first drag

`Main.qml`, the sensor-control `Slider`.

**What was wrong.** `value: parent.current`. A drag assigns `value`
imperatively, which destroys any binding on it, so the binding worked exactly
once; after the first touch the handle kept showing what had been dragged even
when the camera reported something else, and the 1 Hz poll had nothing left to
correct. The quality slider had already solved this with an explicit
`syncFromStatus()` guarded by `!pressed`.

**What changed.** The binding is gone. The delegate's `current` property now
drives `syncFromStatus()` through `onCurrentChanged` and once at
`Component.onCompleted`; the `!pressed` guard stops a poll from yanking the
handle out from under a finger.

#### CP-15 — notifications dismissed by index, which `postOnce` invalidates

`NotificationCenter.{h,cpp}`, `Main.qml`, `tests/tst_notificationcenter.cpp`.

**What was wrong.** `notify.dismiss(notifyCard.index)`. `postOnce()` does
`beginRemoveRows` + `beginInsertRows(0,0)` when a keyed condition recurs and
`sweepExpired()` retires a row every 500 ms, so between a delegate being
created and its × being clicked the captured index may address a different
card — closing somebody else's notification and leaving the clicked one on
screen. This is the exact defect `9beced1` fixed on the C++ side, reintroduced
in QML, because `data()` exposed no key role and QML *could not* use the safe
API.

**What changed:**

- `ItemIdRole` (`itemId`) answers the item's own `m_nextId` identity, which is
  unrelated to the row it occupies — in production `m_nextId` has climbed well
  past `rowCount()`.
- `dismissById(qint64)` is the only dismissal exposed to QML. **`dismiss(int
  row)` was deleted**, not merely avoided: keeping an index-based `Q_INVOKABLE`
  around is how the defect came back the first time, and it has had no
  production caller since `9beced1`.
- The QML delegate declares `required property var itemId` and calls
  `notify.dismissById(notifyCard.itemId)`.

**Test** — `dismissByIdRemovesTheClickedCardNotTheRowUnderneath` advances
`m_nextId` past the row count first (so identity ≠ position is real), captures
`alpha`'s id while it sits at row 1, lets `postOnce()` lift it to row 0, and
then dismisses: the right card goes and `bravo` stays. Out-of-range ids and id
0 are no-ops. The role test also asserts `roleNames()` advertises `itemId`,
because a role that is answered but not advertised leaves every delegate's
required property uninitialised at runtime.

#### CP-23 — the footer's "drop" metric conflated two different counters

`Main.qml`, footer.

**What was wrong.** The label read `stream.framesDropped +
frameBus.overwrittenCount` under the comment "frames the transport discarded".
`MjpegClient.cpp:394` increments `m_dropped` **only** in the decode-failure
branch — it has never counted a transport discard. One number was therefore
carrying two unrelated things and naming a third.

**What changed.** Split into two footer items, each saying what it counts and
each with a tooltip: `dec` for frames that arrived but could not be decoded
(framing/codec), `drop` for frames the bus replaced before the display could
show them (latest-frame-wins). Nothing is summed, so neither is understated.

#### DX-1 — "sign-in required" was rendered as a permanent red error

`SessionState.{h,cpp}`, `CameraDevice.cpp`, `tests/tst_sessionstate.cpp`.

**What was wrong.** `derive()` sets `state = Error` with `severity = "warn"`
for `needsSignIn`, and line 85's own comment says *"Actionable by the user, so
a warning rather than a fault."* `CameraDevice` dispatched on
`state() == State::Error` and ignored severity, posting
`Diagnostics::Level::Error`. `isSticky()` treats `>= Warning` as sticky, so an
ordinary first-run state became a red card the operator had to clear by hand on
every connect, before having done anything wrong.

**What changed.** `SessionState::notificationLevel(severity)` maps the state
machine's own vocabulary onto a level — `"warn"` → `Warning`, everything else →
`Error`, failing closed so a demotion has to be asked for by name. `CameraDevice`
calls it for both the `Error` and `Degraded` branches (Degraded carries `"warn"`
in every branch of `derive()`, so its behaviour is unchanged).

**Test** — `notificationLevelCarriesTheSeverityTheStateSet` derives the
sign-in-required outcome and the rejected-credentials outcome, both
`State::Error`, and asserts they map to `Warning` and `Error` respectively;
plus `Degraded` → `Warning`, and unrecognised/empty severities → `Error`.

**Mutation**

| Mutation | Result |
|---|---|
| `dismissById` compares the row instead of the id (`i != id`) | **exit 1, 14 passed, 1 failed** — `rowCount()` 2, expected 1 |
| `roleNames()` stops advertising `itemId` | **exit 1, 14 passed, 1 failed** — the `roleNames().contains` guard |
| `notificationLevel` always returns `Error` (the pre-fix behaviour) | **exit 1, 20 passed, 1 failed** |
| all restored | exit 0 |

**Gate**

| Check | Result |
|---|---|
| desktop build | exit 0, no errors |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 61.36 s |
| `tst_notificationcenter` | 15/15 |
| `tst_sessionstate` | 21/21 |
| `qmllint` vs `HEAD` (differential) | 18 diagnostics before, 18 after — the 6 differing lines are pre-existing `Quick.layout-positioning` warnings shifted by our edit; **no new diagnostics** |
| runtime QML load | `SortingCamera.exe -platform offscreen` stayed up 8 s with no `objectCreationFailed` and no QML errors on stderr |
| 3 mutations | all caught |

### g5 — CP-11, CP-12, CP-13, CP-21, CP-22: seams nothing used

Five severity-C desktop findings about machinery that existed but was never
exercised, exercised wrongly, or exercised against the wrong target. All five
are software-only; none needs the camera.

**Tracker correction made here.** The `Finding status` rows for CP-11, CP-12
and CP-13 carried summaries that do not match the audit or the source —
"`releaseStale` blocks the GUI thread", "Skip ahead passes no format",
"Frame interval metrics throttled incorrectly". The rows are rewritten below to
what the finding actually says. (The CP-7/CP-8 rows have the same problem and
are corrected in g8.)

#### CP-11 — `releaseStale()` had no production caller

`desktop/src/DeviceRegistry.{h,cpp}`, `tests/tst_deviceregistry.cpp`.

**What was wrong.** The registry destroys idle pipelines by age, and the two
behaviours that matter — the active device is never collected, an unknown
`setActive` is a no-op — are implemented and tested. But `releaseStale()` was
called only from `tst_deviceregistry`. A camera is keyed by its own device id,
so a camera announcing with rotating ids made `acquire()` build one whole
pipeline per id — an `MjpegClient` QThread, a `DeviceStatus` QThread, a
`NotificationCenter` sweep timer — and nothing in the product ever reclaimed
any of them. The list was monotonic.

**What changed.** `acquire()` now sweeps before it constructs a device for a
new id, which is the only event that grows the registry. The TTL is a member
(`kStaleTtlMs`, 60 s) with `setStaleTtlMs()`, so the sweep can be exercised
without waiting out a minute of wall clock; production never touches it.

**Test** — `acquiringANewIdReclaimsTheOnesNobodySelected` sets a zero TTL,
acquires `rotating-1` then `rotating-2` and asserts the first is gone and the
count is still 1; then activates `rotating-2`, acquires `rotating-3`, and
asserts the active one survived. That last part is the rule that makes the
sweep safe: reclaiming the camera on screen would tear down the picture.

#### CP-12 — a gap in the config chain truncated everything after it

`desktop/main.cpp`, new `desktop/src/ConfigWriteSequence.h`,
`tests/tst_configwritesequence.cpp`.

**What was wrong.** A bench run applies its command-line settings one at a
time, each after the previous write's `configBusy` clears. `applyNextConfig()`
switched on a slot index and each case read `if (parser.isSet(opt)) { … }
return false;`. Slot 0 is `--framesize`. A run asking for `--quality` with no
`--framesize` therefore returned false from the first call, the caller read
that as "nothing left", scheduled the stream — and **never wrote the quality at
all**, along with every later option. The run's result file still recorded the
settings that were requested.

**What changed.** The settings asked for become an explicit list before
anything walks it (`ConfigWriteSequence`: slots 0 framesize, 1 quality, 2 xclk,
3 frame-buffer count, 4 grab mode). `configCount` is gone — it is
`configSeq.total()` — and completion is `hasNext()` rather than "how many index
steps were taken", which is what made a gap indistinguishable from the end.

**Tests** — `tst_configwritesequence`, 4 slots:

- `walksEveryRequestedSetting` — all five, in order, then `-1`.
- `aGapDoesNotTruncateTheSequence` — `{1,2}` (the reported defect), a lone
  middle slot `{2}`, and a lone leading slot `{0}`.
- `anEmptyRunHasNothingToDo` — no options at all still schedules the stream.
- `takeNextStopsAtTheEnd` — asking past the end must not wrap, repeat or
  corrupt the count, because the busy handler re-enters this path every time a
  write finishes.

#### CP-13 — frame stats re-ran the session state machine once per frame

`desktop/src/SessionState.{h,cpp}`, `tests/tst_sessionstate.cpp`.

**What was wrong.** `observe()` hooked `MjpegClient::statsChanged`, which
`MjpegClient.cpp` emits on every `readyRead()` — once per incoming frame. No
`SessionInput` field is derived from a frame counter, so each of those calls
ran `recompute()`: fifteen cross-object property reads plus a full `derive()`
building `QString::arg()` detail strings, all discarded by the
"outcome unchanged" early-out. At 20 fps that is 20 wasted recomputes a second
for the life of the stream, more on a busy link; the audit put it at roughly
600/s. `retryDelayMs()` is derived from `retryAttempt`, which is already hooked,
so nothing else was riding along.

**What changed.** The hook is gone, with the reason written where the next
person will read it. `SessionState` gained `recomputeCount()`, because the cost
being removed was the *recompute*, and an outcome spy could not see it: with
the hook present the outcome never changed either, so a `changed()` spy alone
would have passed before and after.

**Test** — `frameStatsNeverChangeTheSessionOutcome` observes real
`MjpegClient`/`DeviceStatus`/`DiscoveryService` objects, fires `statsChanged`
50 times, and asserts **both** that `changed()` did not fire and that
`recomputeCount()` did not move; then `startScanning()` and asserts both did.
The second half is what keeps the first half honest — it proves the wiring is
live and the counter is real, so "nothing happened" cannot be a dead test.

#### CP-21 — an announcement is never checked against the protocol version

`desktop/src/DiscoveryService.{h,cpp}`, `tests/tst_discovery.cpp`.

**What was wrong.** `ingest()` read `proto_version` into
`DiscoveredDevice::protocolVersion` and then did nothing with it. A camera on
another protocol major version was listed, selectable, and driven over control
endpoints, frame framing and field names it does not implement. `docs/protocol.md`
§"Versioning & compatibility" promises the opposite: *"Desktop rejects
major-version mismatch with a clear UI error."*

**What changed.**

- `DiscoveryService::kProtoVersion = 1` and `isCompatible(int)` are the single
  statement of what this build speaks (firmware `PROTO_VERSION` is 1).
- The gate lives in **`ingest()`**, not `parseAnnounce()`. The parser stays a
  pure reader of what a payload says — that is what
  `defaultsAreAppliedWhenFieldsAreAbsent` depends on — and the compatibility
  rule sits where a device would otherwise enter the model.
- Refusal sets `statusText` naming the camera and **both** versions, emits
  `statusTextChanged`, warns once per distinct message, and returns before
  `indexOf()`, so nothing is inserted and `deviceFound` is not emitted. The
  message survives because the "N camera(s) found" overwrite only happens while
  the status still reads `"scanning"`, and `startScanning()` resets it.
- An announcement with **no** `proto_version` parses to 0 and is refused too.
  Unknown is not the same as compatible, and `docs/protocol.md` requires the
  version in every custom header.

**Test** — `rejectsAnAnnounceFromAnotherProtocolVersion`: a v1 camera is listed
and `deviceFound` fires once; the same payload at `proto_version: 2` leaves the
count at 1, is absent from `indexOfDevice`, and `statusText` names the device
plus `version 2` and `version 1`; a third announce at `proto_version: 0` is
refused the same way; `isCompatible(1)/(2)/(0)` are pinned; and `parseAnnounce`
still reports `protocolVersion == 2` for that payload, pinning the
parse/reject split.

**One existing test payload was made realistic** — `fallbackToSenderAddress`
built `{"scam":1,"op":"announce","device_id":"noip"}` by hand and so stated no
protocol version. It now carries `"proto_version":1`. Its subject (falling back
to the UDP sender address) is untouched; it is a better fixture either way.

#### CP-22 — the credential test wrote encrypted passwords to the real registry

`tests/tst_credentialstore.cpp`.

**What was wrong.** `initTestCase` set only the organisation and application
name. Every `QSettings` in `CredentialStore` is default-constructed, which on
Windows is `NativeFormat` — `HKCU\Software\SortingCameraTest`. So a routine
`ctest` wrote real DPAPI-encrypted password blobs into the developer's real
per-user registry and "cleaned up" with `remove()`, which leaves the empty keys
behind.

**What changed.** The same pattern `tst_userprefs` already uses: a `static
QTemporaryDir` and `QSettings::setDefaultFormat(IniFormat)` +
`setPath(UserScope, dir.path())`, issued in `initTestCase` **before the first
`QSettings` exists anywhere in the process**. DPAPI is unaffected — it operates
on bytes, not on where the bytes are kept — so `storedValueIsNotPlaintext` still
proves what it proved.

**Test** — `initTestCase` also probes a default-constructed `QSettings` and
asserts `fileName()` is inside the scratch directory, so the redirection cannot
be silently dropped again.

**Mutation**

| Mutation | Result |
|---|---|
| CP-11: delete the `releaseStale(m_staleTtlMs)` call from `acquire()` | **caught** — `tst_deviceregistry` exit 1, 12 passed, 1 failed (`rotating-1` still present) |
| CP-21: `isCompatible()` always returns `true` | **caught** — `tst_discovery` exit 1, 16 passed, 1 failed |
| CP-22: delete the `setDefaultFormat`/`setPath` redirection | **caught** — `tst_credentialstore` exit 1; *"settings escaped the scratch dir: \HKEY_CURRENT_USER\Software\SortingCameraTest\SortingCameraTest"* — the mutation proves the defect was real, not theoretical |
| CP-12: `hasNext()` only advances while the slots are dense (`m_requested.at(m_applied) == m_applied`) — i.e. restore "an unset slot stops the walk" | **caught** — `tst_configwritesequence` exit 2, 4 passed, 2 failed: `aGapDoesNotTruncateTheSequence` fails on `seq.hasNext()` |
| CP-13: re-add `hook(m_stream, &MjpegClient::statsChanged)` | **caught** — `tst_sessionstate` exit 1, 21 passed, 1 failed on `recomputeCount()` |
| all restored | exit 0 |

**Gate**

| Check | Result |
|---|---|
| desktop build | exit 0, no errors |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 13**, 61.53 s (a 13th suite, `tst_configwritesequence`, was added) |
| `tst_deviceregistry` | 13/13 |
| `tst_discovery` | 17 passed, 0 failed, 2 skipped (the two live-socket tests need `SCAM_DISCOVERY_PORT`) |
| `tst_sessionstate` | 22/22 |
| `tst_credentialstore` | 8/8, and no longer touches HKCU |
| `tst_configwritesequence` | 6/6 |
| 5 mutations | all caught |
| hardware | not required by any finding in this group |

---

### g6 — FW-9, FW-11, FW-12, FW-16, FW-17, FW-19, FW-20: firmware truthfulness

Seven firmware findings about things the firmware either failed to say on the
wire, said wrongly, or did without the policy its neighbours follow. Six are
code changes; FW-17 is a correction of arithmetic in two places.

**A note on evidence, stated up front.** No host-side harness executes the
firmware, so six of the seven are **build-verified only** and stay `fixed`,
never `closed`, until they are confirmed on the camera (the Stage 4 hardware
pass, still pending). The one new desktop test is a *contract* test for FW-9 — it pins what the
client requires of a stream that now ends properly — and is mutation-checked
below. Nothing in this group is claimed as hardware-verified.

#### FW-9 — the MJPEG body never ended

`firmware/.../http_servers.c` `stream_handler()`.

**What was wrong.** Every part is preceded by `--FRAME\r\n`, and when the loop
left, the handler only sent `httpd_resp_send_chunk(req, NULL, 0)` — which
terminates the **chunked transfer**, not the **multipart body**. RFC 2046 wants
`--FRAME--`. `MjpegClient` keys on the literal `"--FRAME"` and so never noticed;
a reader that actually parses delimiters would sit waiting for a boundary that
never arrives.

**What changed.** Before the terminating chunk the handler now sends
`\r\n--FRAME--\r\n` unconditionally. Every exit from the loop is a decision to
leave, so a terminator that itself fails changes nothing — the socket goes with
it.

**Test** — `tst_capture::aCloseDelimiterEndsTheBodyCleanly`: the stub can now
emit the close-delimiter, and the test asserts all four parts decode, no fifth
part appears, `errorString()` stays empty (a malformed tail would populate it
and start the reconnect ladder), and the last frame is still byte-identical.
This is the *contract* side; the firmware side needs the camera.

#### FW-11 — a floor-quality number presented as the requested quality

`firmware/.../camera.c`, `http_servers.c`.

**What was wrong.** `camera_estimate_frame_bytes(fs, quality)` contained
`(void)quality;` and returned the maximum measured **at the resolution's
quality floor**. It was quoted in the below-floor rejection message — "max frame
`%u` B" as though it described the quality the operator just asked for — and in
the config response as `est_frame_bytes`, computed with the *current* quality.
At q36 the real frame is far larger than the floor figure.

**What changed.** Renamed to `camera_measured_max_frame_bytes(fs)` and the
quality parameter deleted: there is no measured size-vs-quality model (ADR-0009
decision 2 measured one, found it wrong, and threw it away), so a number
labelled for a specific quality would be invented, not measured. The rejection
message now says *"largest frame measured at that floor"*, the serial log says
*"measured max frame … at quality N"*, and the JSON key is
`measured_max_frame_bytes` (renamed because no consumer read the old key —
grep across `desktop`, `tools`, `tests`, `docs` found none).

**Verified** — `idf.py build` exit 0; no remaining reference to the old symbol.

#### FW-12 — sensor writes bypassed both the 409 policy and the camera lock

`firmware/.../http_servers.c`, `camera.c`, `app.h`.

**What was wrong.** `config_handler` refuses to reconfigure while
`stream_client_count() + frame_transport_client_count() > 0` and serialises
through `camera_apply_config()`. `sensor_set_handler` did neither: a slider drag
rewrote SCCB registers mid-capture, concurrently with the camera task's DMA, and
could land in the window where `camera_recover()` is between
`esp_camera_deinit()` and `esp_camera_init()`.

**What changed.** The check is now one shared helper, `refuse_while_streaming()`
— same message, same 409, one place to get it wrong — called by `config_handler`
for a param change and by `sensor_set_handler` after the body has been read (so
the connection is drained before it is refused). `camera.c` exports
`camera_lock()` / `camera_unlock()` around `camera_control_apply()`. The drain
**gate** is deliberately *not* closed: a register write does not stop frame
delivery, and closing it would turn every slider nudge into a stream stall.

**Verified by reading, not by running:** `camera_control_apply()` calls only
`esp_camera_sensor_get()` and its own helpers — it never re-takes `s_cam_mutex`,
so the lock cannot self-deadlock. The NVS write-per-request half of the finding
(FW-21) is out of scope for this group and stays open.

#### FW-16 — a password published in git could be auto-provisioned

`firmware/.../CMakeLists.txt`, `config_secrets.example.h`, `auth.c`.

**What was wrong.** The project `CMakeLists.txt` copies
`config_secrets.example.h` to `config_secrets.h` when the latter is missing, and
the example shipped `CAMERA_CONTROL_PASSWORD "CHANGE_ME_CONTROL_PASSWORD"` —
non-empty, so `provision()` succeeded and `s_enabled = true`. A build that was
never given real secrets would report `auth.required: true` while checking
against a value anyone can read in the repository. (The audit cites
`main/CMakeLists.txt`; the copy step is at the project root — line drift.)

**What changed.** The example password is now `""`, which provision() already
treats as "not provisioned" and logs loudly. `auth.c` *additionally* refuses the
old literal by name with `ESP_ERR_NOT_FOUND`, so a stale local copy of
`config_secrets.h` cannot become the real credential silently.

**Checked against this machine:** the local (git-ignored) `config_secrets.h`
holds a 24-character password that is **not** the placeholder, so the new
refusal does not disable auth on this working copy.

#### FW-17 — the socket budget was wrong in both places it was written down

`firmware/.../main/Kconfig.projbuild`, `docs/architecture.md`.

**What was wrong.** The Kconfig help budgeted "7 + 5 = 12 … 14 with the
transport". The source says `max_open_sockets = 6` (control) and `2` (stream),
and `esp_http_server` costs `max_open_sockets + 3` per instance. Real usage is
control 9 + stream 5 + discovery UDP 1 = **15 of `CONFIG_LWIP_MAX_SOCKETS=16`**,
and enabling the transport adds three more — **18 > 16, it does not fit at
all**. `architecture.md` had the same arithmetic error with `max_open_sockets=4`.
The socket-exhaustion argument around that table is correct; only the numbers
were wrong, which is worse than an absent warning.

**What changed.** Both texts now carry the verified arithmetic, discovery is
listed as a consumer, and the consequence bullets say plainly that the transport
requires raising `CONFIG_LWIP_MAX_SOCKETS` to at least 19 rather than implying
there is headroom. Counted directly from source: `discovery.c` opens one UDP
socket; `frame_transport.c` opens a listener, accepts one client, and opens a UDP
socket.

#### FW-19 / FW-20 — loss and pressure that existed only as serial lines

`firmware/.../metrics.c`, `http_servers.c`, `frame_transport.c`;
`docs/protocol.md`, `docs/architecture.md`, `tools/fake_camera.py`.

**What was wrong.** AGENTS.md requires every drop to be counted and never
hidden. Three things were not: a mid-frame send failure in the MJPEG handler and
in the TCP transport logged and broke with no counter; `near_budget_frames` was
a function-local throttle for a `printf`, invisible to the status API; and a
served snapshot incremented nothing, which is *correct* for the liveness
watchdog but undocumented, so the gap against `frames_captured` was
indistinguishable from a bug.

**What changed.** Three mutex-guarded counters — `metrics_record_send_failure`
/ `metrics_record_near_budget` / `metrics_record_snapshot` — exported as
`frames_send_failures`, `frames_near_budget`, `snapshots_served` in the status
JSON and mirrored in the rate-limited log task. The `near_budget_frames` local
is gone; the global is now the throttle, so the "first and every 100th" warning
spans the whole run rather than one connection. `docs/protocol.md` gains a
**Loss counters** table defining each field, including why snapshots stay out of
`frames_captured`; `architecture.md`'s metrics row matches; `fake_camera.py`
emits the same three keys (0, 0, and a real snapshot count).

**Mutation / contract check**

| Mutation | Result |
|---|---|
| stub emits a fifth part *after* the close-delimiter | **caught** — `tst_capture` exit 1, 10 passed, 1 failed: `aCloseDelimiterEndsTheBodyCleanly`, `bus.version()` actual **5**, expected 4 (`tests/tst_capture.cpp:551`, reported as 552 in the mutated build) |
| restored | exit 0, 11 passed, 0 failed |

That mutation proves the new assertion can fail. It does **not** prove FW-9
fixed: the firmware side of FW-9 is not executed by any test in this repository,
and no mutation of the firmware is available without the camera. Recorded as
such rather than presented as coverage.

**Gate**

| Check | Result |
|---|---|
| `idf.py build` (ESP-IDF 5.5.4, after all seven edits) | **exit 0**, `esp32_cam_stream.bin` 0xf7bc0, 52% of app partition free |
| desktop build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 13**, 61.98 s (`tst_capture` 47.85 s) |
| `tst_capture` | 11 passed, 0 failed, 1 skipped (live-device test needs `SCAM_TEST_HOST`) |
| 1 mutation | caught, then restored to green |
| stale-symbol grep | no `camera_estimate_frame_bytes` / `est_frame_bytes` outside the frozen audit |
| hardware | **not run** — FW-9, FW-11, FW-12, FW-16 need the camera; FW-17 and the doc changes do not |

---

### g7 — FW-13, FW-10: config writes are POST, and the silent 200 is gone

Two findings about one endpoint, resolved together because fixing either alone
leaves the other's failure mode: making writes POST without removing the
fall-through leaves a GET that reports success for a camera nobody touched, and
fixing only the truncation leaves state-changing GETs on the wire.

**A note on evidence, stated up front.** The firmware half is **build-verified
only** and stays `fixed`, never `closed`, until it runs on the camera. What is
verified here is the *contract*: the desktop's assertions were rewritten to pin
the new request shape, and the fake camera implements the same four refusal
paths in the same order as the firmware, so a client that passes against the
fake meets the firmware's rules. Recorded as such rather than as hardware
coverage. Decision recorded in **ADR-0017**.

#### FW-13 — the config endpoint took writes on GET

`firmware/.../http_servers.c`, `desktop/src/DeviceStatus.{h,cpp}`,
`benchmarks/run_phase3.py`, `benchmarks/phase4_matrix.py`,
`tools/fake_camera.py`.

**What was wrong.** `/api/v1/config` was registered `HTTP_GET` while
`docs/protocol.md` said `GET/PUT`. Every configuration change was a URL: the
desktop sent `GET .../config?framesize=hd&quality=12`, `set_config()` and
`apply_config()` did the same, and the fake camera applied whatever the query
string carried. That is a state change reachable by a request a browser will
send without asking, on a device that answers every preflight with 405 and
serves `Access-Control-Allow-Origin: *`.

**What changed.**

- `config_handler` was split into `config_get_handler` (read-only) and
  `config_post_handler` (the write), both registered on `/api/v1/config`;
  `esp_http_server` dispatches on URI *and* method, so the two cannot catch
  each other's requests and an unmatched method still returns 405. Both share
  one `config_state_reply()` so their responses cannot drift apart.
- The POST handler requires a `Content-Type` containing `application/json`
  (**415** otherwise), reads a 1–512-byte JSON object, rejects a query string,
  an unknown key, a wrong type or an out-of-range number with **400** *before*
  anything is applied, then runs the same 409 stream gate, atomic apply and
  rollback the GET used to run.
- The desktop's setters build a `QJsonObject` instead of `"key=value"` strings;
  `setConfigQuery`/`m_cfgQuery`/`m_pendingKv` became
  `setConfigBody`/`m_cfgBody`/`QMap<QString, QJsonValue>`, so `quality` leaves
  as the number 12 and not the string `"12"`. The 401-replay machinery (CP-6)
  is untouched: `m_cfgBody` is what gets replayed.
- Both benchmark harnesses and `fake_camera.py` moved to POST JSON.
  `benchmarks/results/**` — recorded runs — were not modified.

#### FW-10 — a truncated config query returned 200 OK

`firmware/.../http_servers.c`.

**What was wrong.** `httpd_req_get_url_query_str(req, query, 192)` returns
`ESP_ERR_HTTPD_RESULT_TRUNC` when the query exceeds 191 bytes
(`httpd_parse.c:992`), and the old handler only acted on `== ESP_OK`. A
truncated query therefore skipped the whole apply block and fell through to the
JSON echo: **200 OK** describing an unchanged camera. The same fall-through
answered `200` for a URL with no query at all, which is how a bare GET read
state — so the "success" path and the "nothing happened" path were the same
line. `char val[16]` was the quieter cousin: a value longer than 15 characters
was cut before `atoi()` saw it.

**What changed.** The GET handler now distinguishes all three cases:
`ESP_ERR_HTTPD_RESULT_TRUNC` → **400** `config query too long`; `ESP_OK` (a
query exists) → **400** naming the POST alternative; anything else → the state
JSON. On the POST side the query string is rejected too, after the body is
drained so the request is fully consumed before the answer is written. Values
now arrive as JSON in a body sized and parsed as a whole, so there is no
truncating `val[16]` and no partial `atoi()`.

**Mutation / contract check**

| Check | Result |
|---|---|
| mutation A — `doConfig()` reverts to `m_nam->get(request)` | **caught** — `tst_devicestatus` exit 2, 11 passed, 2 failed (the method/path assertions in `aProfileLeavesAsASingleConfigRequest`) |
| mutation B — the `Content-Type: application/json` header is dropped | **caught** — exit 1, 12 passed, 1 failed (`content-type` assertion; the stub records headers for this purpose) |
| restored | exit 0, **13 passed, 0 failed** |
| fake-camera contract smoke (15 cases: read-only GET, GET/POST with query → 400, missing and `text/plain` content type → 415, unknown key → 400 naming it, non-object body → 400, accepted write echoes and persists, status unaffected) | **15/15 pass**, exit 0 |

Both mutations prove the new assertions can fail. Neither proves the *firmware*
enforces the contract: no mutation of `http_servers.c` is available without the
camera. The fake camera implements the same four refusals in the same order, so
a client that passes against the fake meets the firmware's rules — recorded as
such rather than presented as coverage of the device.

**Gate**

| Check | Result |
|---|---|
| `idf.py build` (ESP-IDF 5.5.4, after both handlers + the split registration) | **exit 0**, `Project build complete` |
| desktop build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 13**, 62.08 s (`tst_devicestatus` 8.89 s, `tst_capture` 48.13 s) |
| 2 desktop mutations | both caught, then restored to green (13/13) |
| fake-camera contract smoke | 15/15 |
| `python -m py_compile` on both harnesses and the fake | exit 0 |
| stale-symbol grep | no `setConfigQuery` / `m_cfgQuery` / `config?` left outside the frozen audit |
| hardware | **not run** — FW-13 and FW-10 are firmware behaviour; needs the camera |

---

### g8 — CP-7, CP-8, CP-9: the stream tests never met the shipped wire; FW-18: the split is written down

Three test-matrix findings about `tst_capture` plus one documentation finding.
The implementation was correct in every case — the audit says so for CP-7 and
CP-9 — so what was missing was proof: nothing in CI would notice if a delay
table, the chunked decoder or one of the parser bounds changed underneath the
published contract (`docs/protocol.md` → *Reconnect contract* and *MJPEG
framing*). FW-18 is documentation only; the audit states "no change required
beyond a written rationale".

**One production change was needed to write the tests at all**, and it is the
only code change outside `tests/`: `MjpegClient` gained a `lastErrorString`
property. The retry path clears `errorString()` before the next attempt — that
is the published behaviour, so the UI never keeps showing a stale reason — which
left no way for a test, or an operator reading diagnostics, to see *which*
branch fired while the ladder was still running. `lastErrorString` is recorded
in the `errorUpdated` handler just before the clear and is reset by `start()`.
It changes no behaviour, only reports it.

#### CP-7 — the reconnect ladder and both watchdogs were never exercised

`tests/tst_capture.cpp`, `desktop/src/MjpegClient.{h,cpp}`.

**What was wrong.** The ladder is `kMaxRetries=5` with delays
`{500,1000,2000,3000,5000}` ms, reset only on a *decoded* frame, plus a 6 s
first-byte watchdog and an 8 s stall watchdog — the §24 "no reconnect storms"
mechanism that stops a dead camera from masquerading as a live stream. No test
touched it: `MjpegStub` never failed, never stalled, never closed. A regression
that advanced the counter on TCP connect, or shipped a different delay table,
went green.

**What changed.**

- `theRetryLadderSpendsEveryStepThenGivesUp` — every connection is answered
  with a response head that never terminates (`"HTTP/1.1 200 OK\r\n"` plus 9000
  `X`s), so each attempt fails for the same reason as fast as the parser
  allows. The test pins `maxRetries() == 5`, then asserts the recorded
  `retryAttemptChanged` pairs are exactly attempts `1..5` with delays
  `500/1000/2000/3000/5000` ms; that after the *sixth* connection's failure
  `isReconnecting()` is false (no seventh retry was scheduled) and the reason is
  still in `lastErrorString()`; and that a further 600 ms wait leaves
  `retryAttempt()` at 5. The constants are the shipped ones — the test never
  shortens them, it only makes each attempt fail immediately instead of after a
  watchdog.
- `aDecodedFrameResetsTheRetryLadder` — after a first failure (attempt 1,
  500 ms) the stub is switched back to healthy, and one decoded frame must put
  `retryAttempt` back to 0; the failure *after* that must start again at attempt
  1 / 500 ms rather than carrying the earlier step forward, with the reason
  (`Content-Length`) visible in `lastErrorString()`.

#### CP-8 — the tested wire format was not the shipped wire format

**What was wrong.** The stub emitted `multipart/x-mixed-replace` parts with
`Content-Length` and **no** `Transfer-Encoding`, so the parser ran only its
`HttpState::Identity` branch. The firmware sends every frame with
`httpd_resp_send_chunk` → `Transfer-Encoding: chunked`, so the
`ChunkSize`/`ChunkData`/`ChunkEnd` states — the path the real device uses on
every frame — were executed by nothing in CI.

**What changed.** `MjpegStub` gained a `chunked` mode (and a `rawResponse`
escape hatch used by the cases below). Each part is written as a transfer chunk
whose size line is **split inside the `Content-Length` digits** (`encodeChunked()`
cuts at `head.indexOf("Content-Length: ") + 17`, one digit in). That is what
makes the test discriminating: an identity reader takes `1` as the whole frame
and cannot decode it.
`aChunkedStreamDecodesTheBytesTheFirmwareSends` asserts 4 frames received,
**0 dropped**, no error, and `sha256` of each emitted JPEG equals the bytes the
stub was handed — the byte-identity rule AGENTS.md sets for snapshots and
recordings, applied to the decode path.

#### CP-9 — no malformed-input matrix; six `fail()` branches were dead to CI

**What was wrong.** The bounds are real and were verified by reading the code —
8 MiB buffer, 8 KiB response headers, 2 KiB part headers, 128 B chunk line — but
had no regression guard, so the "bounded buffers" claim in
`docs/architecture.md` and AGENTS.md was true and untested.

**What changed.**

- `malformedStreamsFailWithTheNamedError` — seven cases, each the byte string
  that trips one bound: response headers over budget → `malformed HTTP response
  headers`; part headers over budget → `malformed part headers`; chunk size line
  over 128 B → `malformed chunk size line`; `xyz` → `bad chunk size`; a
  terminator that is not CRLF → `malformed chunk terminator`; `Content-Length:
  nope` → `missing or invalid Content-Length`; a part declaring 9,000,000 bytes
  carrying 8.5 MiB → `stream buffer overflow`. The assertion is on the **named
  reason**, which is what proves the right branch fired rather than some branch.
- `aForeignBoundaryNeverMatchesAndTheStallWatchdogEndsTheStream` — the
  `boundary` parameter is deliberately ignored (the firmware writes `--FRAME`;
  trusting a camera-supplied boundary would hand a remote string to a parser), so
  a stream declaring `boundary=NOTFRAME` matches nothing. What has to happen is
  bounded and finite: the 8 s stall watchdog ends it with `stream stalled` and
  `8000 ms`, with `framesReceived() == 0`, `framesDropped() == 0` and
  `bytesReceived() > 0` — the data never became a part, and neither a hang nor
  an unbounded buffer happened.

#### FW-18 — the stream/snapshot split had no written rationale

`docs/protocol.md`, `docs/architecture.md`.

**What was recorded.** `stream_handler` never calls `auth_authorized` while
`snapshot_handler` does, the capabilities response honestly reports
`stream_port_protected: false`, and §19 excludes video-stream encryption — but
the asymmetry with the snapshot was nowhere written down, so it read as a bug
from the status JSON. `docs/protocol.md` now carries a *Stream authentication
posture (FW-18)* bullet and `docs/architecture.md`'s *Authentication* section
explains it next to the token rules: the control plane is closed uniformly and a
snapshot sits on it, the video plane has no TLS so a token would travel in clear
text against a listener who can already read the frames it guards, and the split
is advertised rather than hidden. Revisit triggers (TLS on the video plane with
its measured cost/benefit; an untrusted or multi-tenant network) are stated with
it. **No code change** — the audit says none is required.

**Mutation check**

| Check | Result |
|---|---|
| M1 — `frameReady` handler no longer resets the ladder to step 0 | **caught** — `aDecodedFrameResetsTheRetryLadder` fails, exit 1 (2 passed, 1 failed) |
| M2 — one retry delay 2000 → 2500 ms | **caught** — `theRetryLadderSpendsEveryStepThenGivesUp` fails, exit 1 |
| M3 — chunked detection disabled (`m_chunked = false;`) | **caught** — `aChunkedStreamDecodesTheBytesTheFirmwareSends` fails, exit 1 |
| M4 — 8 MiB buffer budget raised to 64 MiB | **caught** — `malformedStreamsFailWithTheNamedError` fails, exit 1 |
| restored | exit 0, **16 passed, 0 failed, 1 skipped**, 70360 ms |

Each mutation is paired with exactly the test written for it, so a non-zero exit
cannot come from an unrelated failure. Qt 6.11 rejects `-function` as an unknown
option, so the single-test runs pass the function name as a positional argument
(init + cleanup + the one test = "2 passed, 1 failed").

**Gate**

| Check | Result |
|---|---|
| desktop build (Ninja, MinGW 13.1 / Qt 6.11.2) | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 13**, 84.37 s final confirmation run (82.70 s on the first g8 run, `tst_capture` 69.83 s) |
| `tst_capture` | **16 passed, 0 failed, 1 skipped** (was 11 passed) — 5 new slots |
| 4 mutations | all caught by their own test, then restored to green |
| fake-camera contract smoke, committed as `tools/config_contract_smoke.py` | **18/18**, exit 0 — the g7 control-plane 15 plus 3 wire cases reading the fake's chunked stream (CP-8: "started by nothing") |
| `python -m py_compile tools/config_contract_smoke.py` | exit 0 |
| firmware | **not touched** — CP-7/8/9 are desktop test gaps, FW-18 is documentation; no `idf.py build` needed |
| hardware | **not run** — nothing here changes device behaviour |

### g9 — the last open rows (FW-2, FW-4, FW-8, DX-12, DX-17), the never-triaged FW-14 / CP-20, and the documentation sweep (2026-09-30)

Stage 4 closes with the five rows that were still `open`, plus the two audit IDs
that had never been given a row here at all. Everything else in this pass is
documentation: the drift table (DD-*), the ADR-0009 number the firmware review
caught, and the triage of every remaining ID so that **no audit finding is
untracked** — 42 IDs were missing from this table and are dispositioned in
[Finding status](#finding-status) below.

#### Firmware: bounds that were missing (FW-2, FW-4, FW-8, FW-14)

**FW-2 — a discovery query reached `cJSON_Parse` unvalidated, on a 4 KB stack.**
`discovery.c` received up to `DISCOVERY_MAX_PACKET` (512) bytes from an
unauthenticated UDP sender and parsed them directly; cJSON recurses once per
nesting level, and `CJSON_NESTING_LIMIT` bounds the *buffer length*, not the
call stack — a datagram of open brackets was enough to walk into the FreeRTOS
stack canary. The fix is a check **before** the parser: `json_depth_within()`,
an iterative walk that counts brackets, skips string bodies (so a `{` inside a
value is not structure) and refuses an unbalanced closer, returning false past
`DISCOVERY_JSON_MAX_DEPTH` (8 — a real query is one object deep). The packet is
dropped with `ESP_LOGW` and no reply. No recursion of its own, so the check
cannot become the thing that overflows.

**FW-4 — the TCP/UDP transport retried capture without a bound.** The TCP loop
gave up after `++fails > 50` (the ~225 s stall ADR-0009 §4 removed from the HTTP
handler, still present here), and the UDP loop had no budget at all. Both now use
the same policy the stream handler has used since ADR-0009: one failed capture →
up to **2** `camera_recover()` attempts → close this client (`STREAM_CAPTURE_FAIL_LIMIT`
= 1 in `app.h`), mirroring `http_servers.c:453-484`. The UDP loop additionally
clears `s_udp_peer` and `peer_was_active` once its budget is spent — "dropping
peer" — so a wedged peer stops being retried forever.

**FW-8 — the no-capture watchdog reset the camera at 1 Hz, forever.** The branch
reads `metrics_us_since_last_capture`, which only a real capture advances; a
camera that re-initialised cleanly but never delivered a frame therefore
regenerated its own trigger on every 1 s tick — a permanent, silent power-cycle
loop with no counter going anywhere. Recovery is now bounded and spaced:
`CAMERA_NO_CAPTURE_RECOVERY_LIMIT` (3) attempts, one full `CAMERA_DEAD_TIMEOUT_US`
window apart, and after the third the camera is **left down with an error log**.
Leaving it down is the decision, and it is written into `docs/architecture.md`
(→ *Camera failure model and recovery*): the control plane stays up, `/status`
still reports `camera_up=0` and `camera_recoveries`, and a config apply re-runs
recovery — an operator can see and act on a camera that stays dead instead of
watching it reboot a thousand times. The stall branch (client attached) is
unchanged.

**FW-14 — `to_hex()` took an output length and ignored it.** It wrote 33 bytes
and the caller then discarded the length (`(void)token_len`), which is one
format edit away from an overflow for the next caller. `to_hex()` is now
`static bool to_hex(..., size_t out_len)` and fails when `out_len < len*2+1`;
`auth_verify_login` checks `token_len` against the slot token size **before**
`esp_fill_random`, clears `slot->used` on failure and returns an error string
("token buffer too small"). The single caller passes `char token[64]`
(`http_servers.c:755`), so no behaviour changes today — the API simply cannot be
used wrongly any more.

#### Desktop: build structure and a measured teardown bound (DX-12, DX-17)

**DX-12 — the image provider was outside the library it belongs to.**
`FrameImageProvider.cpp/.h` moved from `qt_add_executable(SortingCamera …)` into
`qt_add_library(scamcore …)`, with `Qt6::Quick` added to scamcore's link line,
so everything below the QML layer is one target again. Full rebuild exit 0
(85 steps).

**DX-17 — the blocking teardown was asserted by comment, not measured.**
`MjpegClient` and `DeviceStatus` stop their worker with a `BlockingQueuedConnection`
plus `QThread::wait()` from the GUI thread. The structure is deliberate (the
worker owns socket, timers and parser) and what makes it acceptable is that the
wait is bounded — `stop()`/`stopPolling()` only stop timers and abort a socket.
Two new tests now *time* it with work in flight: `tst_capture.teardownOfALiveStreamStaysWithinTheGuiBudget`
(streamed ≥2 frames, teardown inside a `QElapsedTimer`, budget 200 ms) and
`tst_devicestatus.teardownWhilePollingStaysWithinTheGuiBudget` (polling against
the stub, same budget). **Measured: 2 ms and 1 ms.** The 200 ms figure is the
stated threshold above which this stops being invisible and starts reading as a
frozen UI; the bound is written up in `docs/architecture.md` → *Threading*.

#### CP-20 — the protocol doc claimed the device reports an app version

It does not and should not: `discovery.c` and `http_servers.c` report
`fw_version` + `proto_version` only (`app_version` appears nowhere in firmware or
desktop source), because a camera cannot know what program is driving it. The
**device side was right, `docs/protocol.md` was wrong** — corrected to say the
app version is desktop-side and is recorded in the recording header and in
benchmark results.

#### Documentation sweep (DD-1 … DD-13, plus the ADR-0009 number)

| Item | What changed |
|---|---|
| DD-1 transport | `ADR-0007` status now records **decided, not implemented**, with a dated amendment (no desktop TCP/UDP client has ever existed; `frame_transport.c` is compiled in but `CONFIG_SORTING_CAM_FRAME_TRANSPORT` defaults to `n`); measured basis untouched. `architecture.md`'s `transport` row and `protocol.md`'s status line now say **HTTP MJPEG is what ships** |
| DD-2 topology | Master Prompt §5 carries a superseded-by-ADR-0006 banner (softAP, `192.168.4.1`, one network per camera), with the original text kept for provenance |
| DD-3 thread model | **verified true, not edited**: post-CP-3 `CameraDevice.cpp:184` connects `frameReady` with `Qt::DirectConnection`, so the `net thread writes FrameBus` / `Record on net thread` rows are accurate |
| DD-4 socket count | already corrected in g6 (15 of 16 baseline, 18 with the transport) |
| DD-5 `architecture.md` status | rewritten to the current phase with pointers to the results and this tracker |
| DD-6 module names | the module table now maps every logical module to its source file (`camera.c`, `wifi.c`, `http_servers.c`, `frame_transport.c`, `discovery.c`, `metrics.c`, `app_main.c`, `auth.c`) |
| DD-7 production default | the stale sentence is gone; `benchmark-results.md` already carries the ADR-0010 forward pointers |
| DD-8 `performance.md` status | now says the numbers are **targets** and points at `benchmark-results.md` |
| DD-9 ESP-IDF pin | `licensing.md` row: **v5.5.4**, pinned 2026-09-23 per ADR-0005, installed by `scripts/install-idf.ps1` |
| DD-10 testing claims | `tst_credentialstore`'s row now lists exactly its five tests and names wrong-password/storage-unavailable as **not covered**; `tst_capture`'s row no longer says "byte identity on the wire" — it compares against what the client's own receive path delivered |
| DD-11 SSID | `protocol.md` gains *Device identity & discovery*: SSID `ESP32-CAM`, fixed IP, one network per camera, UDP announce on 48888, 2 s query, 250 ms reply limit, dedupe by device id |
| DD-12 "verified against 0.1.0" | now says the note records **which build** the observation was taken on and points at CP-24 |
| DD-13 duplicate prompts | the byte-identical-after-CRLF hyphen copy is **deleted** (`git rm`); the em-dash file AGENTS.md names is the only one left, and §5 was amended in it |
| DD-14 `.gitignore` breadth | **left to the owner** — that file already carries an uncommitted owner edit in the working tree; it is never staged here |
| ADR-0009 budget typo | "256,144-byte budget (256 KiB)" → **262,144** (what `sdkconfig.defaults` and the code use), with a note so nobody corrects the firmware to match the typo — audit §*Documentation nit the firmware review caught* |

#### Triage of the 42 IDs that had no row

Every audit ID now appears in [Finding status](#finding-status). The classes and
where they landed:

- **`CP-20` → closed** (doc corrected, this section), **`FW-14` → fixed** (build-verified,
  hardware pending) — the two that had never been triaged.
- **`CP-24`** (version reaches the wire via the IDF default) → **Stage 5**, with the
  release/version single-sourcing work (G-10).
- **`G-1 … G-13`** → Stage 5 for the packaging/CI/README/accessibility items
  (G-1, G-2, G-4 … G-13), **G-3 (the ≥1 h soak gate) → Phase 8 / D1**, which is
  where the hardware and the 60-minute run live.
- **`DD-1 … DD-14`** → the sweep table above (DD-14 to the owner).
- **`S-1 … S-4`** → **accepted residual security findings**, Group E, optional by the
  audit's own severity (P2/P3 on a softAP whose adversary is a joined client);
  each keeps its row so it is a decision on the page, not an omission.
  **S-5** (unauthenticated video stream) is already `closed` as FW-18.
- **`FW-21 … FW-27`** → the audit's own *"Remaining firmware findings, individually
  lower severity"* table: **deferred, P3**, several needing hardware to observe.
  They keep rows so the deferral is visible; none of them is claimed fixed.

#### Evidence

| Check | Result |
|---|---|
| `idf.py build` (after FW-2, FW-4, FW-8, FW-14) | **exit 0**, no warnings; `esp32_cam_stream.bin` 0xf80a0; `app_main.c`/`discovery.c`/`frame_transport.c`/`auth.c` recompiled |
| desktop configure + full rebuild (after DX-12) | **exit 0** (85 steps; one pre-existing `qfile.h` `QFILE_MAYBE_NODISCARD` note, not from this change) |
| `ctest --test-dir desktop\build --output-on-failure` | **13/13**, 83.15 s |
| `tst_capture` | **17 passed, 0 failed, 1 skipped** (16 + the new teardown test; 70.75 s) |
| `tst_devicestatus` | **14 passed, 0 failed** (13 + the new teardown test; 8.98 s) |
| teardown measurements | `MjpegClient` **2 ms**, `DeviceStatus` **1 ms** (budget 200 ms) |
| `tools/config_contract_smoke.py` | 18/18 (g8, unchanged by this pass) |
| hardware / live device | **not run** — the PC is not joined to the `ESP32-CAM` softAP; FW-2, FW-4, FW-8 and FW-14 stay `fixed`, not `closed` |

---

## Finding status

| ID | Sev | Area | Summary | Status | Evidence |
|---|---|---|---|---|---|
| CP-1 | A | desktop | Test suites never registered | closed | Stage 1: `ctest -N` 12, `ctest` 12/12 |
| CP-2 | A | desktop | `ProfileEngine` borrows another profile's byte count | **closed** | Stage 3: 16982 (own config), 14680 rejected by test; mutation-tested |
| CP-3 | A | desktop | Recording and frame bus run on the GUI thread | **closed** | Stage 4 g1: Direct frame hop + `DirectConnection` delivery + synchronised `Recorder`; mutation-caught |
| CP-4 | A | firmware | Sensor controls reset on every `esp_camera_init` | **fixed** | Stage 4 g3: restore moved into `camera_driver_init()`, boot call removed; build-verified, hardware pending g5 |
| CP-5 | A | desktop | Recovery counter never resets | **closed** | Stage 4 g3: re-arm at the 3 `setRecoveryHint` sites; 3-episode test; mutation-caught (exit 1) |
| CP-6 | A | desktop | 401 on a config write silently drops the change | **closed** | Stage 4 g2: one bounded replay after `AuthClient::settled()`; 2 new tests; mutation-caught (11/13, exit 2) |
| CP-7 | B | tests | Session-reconnect watchdogs and the retry ladder never tested | **closed** | Stage 4 g8: 2 new tests pin the shipped 5-step delay table (500/1000/2000/3000/5000 ms), the give-up, and reset-on-decoded-frame; 2 mutations caught |
| CP-8 | B | tests | Tested wire format is not the shipped (chunked) format | **closed** | Stage 4 g8: stub gains chunked mode split inside the `Content-Length` digits; byte-identity decode test; chunked-detection mutation caught; fake's chunked stream now read by `tools/config_contract_smoke.py` |
| CP-9 | B | tests | No malformed-frame matrix; six `fail()` branches dead to CI | **closed** | Stage 4 g8: 7-case named-reason matrix + foreign-boundary/stall test; buffer-budget mutation caught |
| CP-10 | C | QML | Overlapping anchored labels | **closed** | Stage 4 g4: three labels wrapped in a `Column`; qmllint differential clean, runtime load clean |
| CP-11 | C | desktop | `releaseStale()` has no production caller | **closed** | Stage 4 g5: sweep on new-id `acquire()` + `setStaleTtlMs`; new test; mutation-caught |
| CP-12 | C | desktop | Config chain stops at the first unset option | **closed** | Stage 4 g5: `ConfigWriteSequence` + new suite; 4 tests; mutation-caught (exit 2) |
| CP-13 | C | desktop | Frame stats hook re-runs `recompute()` per frame | **closed** | Stage 4 g5: hook removed + `recomputeCount()`; new test; mutation-caught |
| CP-14 | C | QML | Slider binding reads `parent.current` | **closed** | Stage 4 g4: `syncFromStatus()` + `onCurrentChanged`, `!pressed` guarded; qmllint differential clean |
| CP-15 | C | QML | Dismisses by index, not by identity | **closed** | Stage 4 g4: `ItemIdRole` + `dismissById`, `dismiss(int)` deleted; new test, 3 mutations caught |
| CP-16 | B | desktop | 7 fps floor presented as measured | **closed** | Stage 3: `floorProvisional` + `activeFloorProvisional`; stale doc section replaced |
| CP-17 | B | desktop | D2 evidence string overstates the run | **closed** | Stage 3: alt 9.97, 28,956 B, "one run measured three ways"; ADR-0012 corrected |
| CP-18 | B | firmware | NVS restore trusts types with no validation | **fixed** | Stage 4 g3: enum + floor/ceiling checks in `camera_cfg_restore()`; build-verified, hardware pending g5 |
| CP-19 | A | firmware | Frame mutex held across network send | **fixed** | Stage 4 g1: drain gate in `camera.c` (ADR-0016); `idf.py build` exit 0; hardware validation pending (Stage 4 g5) |
| CP-20 | B | docs | `protocol.md` claimed the device reports an app version | **closed** | Stage 4 g9: bullet corrected — firmware reports `fw_version` + `proto_version` only (`discovery.c:91-101`, `http_servers.c:56,601`); app version is desktop-side, recorded in the recording header and benchmark results. The code was always right |
| CP-21 | C | desktop | Protocol version check absent | **closed** | Stage 4 g5: `kProtoVersion` gate in `ingest()` + `statusText` naming both; new test; mutation-caught |
| CP-22 | C | desktop | Test settings path not redirected | **closed** | Stage 4 g5: scratch-dir `QSettings` in `initTestCase` + path probe; mutation-caught |
| CP-23 | C | QML | Decode-failure counter labelled as transport drops | **closed** | Stage 4 g4: `dec` and `drop` shown separately with tooltips, no sum |
| CP-24 | C | firmware | Firmware version reaches the wire via the IDF default | open | Stage 5 (B5): version single-sourcing, together with G-10 |
| CP-25 | B | desktop | Conservative reading rule documented but not implemented | **closed** | Stage 3: `conservativeFps()` used by `recommended()` and `activeMeetsFloor()`; disagreement forced by test |
| DX-1 | C | QML | Severity conflated for sign-in failure | **closed** | Stage 4 g4: `SessionState::notificationLevel()` consulted by `CameraDevice`; new test, mutation caught |
| DX-12 | D | desktop | `FrameImageProvider` outside `scamcore` | **closed** | Stage 4 g9: provider moved into `qt_add_library(scamcore)` + `Qt6::Quick` linked there; full rebuild exit 0 (85 steps) |
| DX-17 | D | desktop | `~CameraDevice` blocking-queued across threads | **closed** | Stage 4 g9: bound **measured**, 2 tests — `MjpegClient` teardown 2 ms, `DeviceStatus` teardown 1 ms against a 200 ms budget; written up in `architecture.md` → *Threading* |
| A0 | A | firmware | PSRAM config unreproducible | **closed** | ADR-0015: clean config == measured config; generated `sdkconfig` diff IDENTICAL |
| FW-1 | A | firmware | Control reset on re-init (alias of CP-4) | **fixed** | Stage 4 g3 (CP-4); build-verified, hardware pending g5 |
| FW-2 | C | firmware | Discovery accepts oversized query | **fixed** | Stage 4 g9: `json_depth_within()` refuses nesting > 8 (iterative, string-aware, refuses unbalanced closers) before `cJSON_Parse`, logs and drops; `idf.py build` exit 0; hardware pending |
| FW-3 | A | firmware | PSRAM config divergence | **closed** | No divergence exists — see ADR-0015; `80M` was silently discarded, `40M` measured and produced |
| FW-4 | B | firmware | Unbounded recovery on stream failure | **fixed** | Stage 4 g9: `frame_transport.c` TCP and UDP loops bounded to 1 failed capture + 2 `camera_recover()` attempts then close (mirrors `http_servers.c:453-484`); UDP exhausts to `s_udp_peer = 0` ("dropping peer"); `idf.py build` exit 0; hardware pending |
| FW-6 | B | firmware | NVS restore unvalidated (alias of CP-18) | **fixed** | Stage 4 g3 (CP-18); build-verified, hardware pending g5 |
| FW-7 | A | firmware | Camera lock held across send (alias of CP-19) | **fixed** | Stage 4 g1 part 2: same change as CP-19; see ADR-0016 |
| FW-8 | B | firmware | Watchdog resets camera at 1 Hz | **fixed** | Stage 4 g9: no-capture branch bounded to `CAMERA_NO_CAPTURE_RECOVERY_LIMIT` (3), spaced a full dead-time window apart, then leaves the camera down with an error log (control plane stays up); stall branch unchanged; decision written up in `architecture.md`; `idf.py build` exit 0; hardware pending |
| FW-9 | C | firmware | Missing close delimiter | **fixed** | Stage 4 g6: `\r\n--FRAME--\r\n` before the terminating chunk; client contract test + mutation; **hardware pending** |
| FW-10 | B | firmware | Truncated config query returns 200 OK | **fixed** | Stage 4 g7: GET distinguishes `TRUNC` → 400, query present → 400, else state; POST refuses a query after draining the body; ADR-0017; build-verified, hardware pending |
| FW-11 | C | firmware | Quality-blind frame estimate | **fixed** | Stage 4 g6: `camera_measured_max_frame_bytes(fs)`, quality parameter deleted, honest messages, JSON key renamed; build-verified, hardware pending |
| FW-12 | C | firmware | Sensor endpoint lacks lock and debounce | **fixed** | Stage 4 g6: shared `refuse_while_streaming()` 409 + exported `camera_lock()`/`camera_unlock()`; build-verified, hardware pending. The NVS-per-request half is FW-21, still open |
| FW-13 | B | firmware | Config accepts GET, spec says POST | **fixed** | Stage 4 g7: `config_get_handler` (read-only) + `config_post_handler` (JSON body, 415 on wrong content type, 400 on query/unknown key); desktop, both harnesses and the fake moved in the same change; ADR-0017; build-verified, hardware pending |
| FW-14 | C | firmware | `to_hex()` ignored its output length | **fixed** | Stage 4 g9: `static bool to_hex(..., size_t out_len)` refuses a short buffer; `auth_verify_login` checks `token_len` before `esp_fill_random` and clears `slot->used`; sole caller passes `char token[64]` so no behaviour change; `idf.py build` exit 0; hardware pending |
| FW-16 | C | firmware | Path cited for secrets check is wrong | **fixed** | Stage 4 g6: example password emptied **and** `provision()` refuses the old literal; the copy step is in the project-root `CMakeLists.txt`, not `main/`; build-verified, hardware pending |
| FW-17 | C | firmware | Socket budget in three places, three answers | **fixed** | Stage 4 g6: Kconfig help + `architecture.md` corrected to 15 of 16 at baseline, 18 with the transport (it does not fit); docs-only |
| FW-18 | D | firmware | Video stream unauthenticated; snapshot is not (disclosed) | **closed** | Stage 4 g8: written rationale in `docs/protocol.md` (*Stream authentication posture*) and `docs/architecture.md` → *Authentication*, with revisit triggers; audit states no code change required |
| FW-19 | D | firmware | Counter definition undocumented | **fixed** | Stage 4 g6: `frames_send_failures` / `frames_near_budget` / `snapshots_served` counted and defined in `docs/protocol.md`; build-verified, hardware pending |
| FW-20 | D | firmware | Documented counter not implemented | **fixed** | Stage 4 g6: all three exported in the status JSON, `fake_camera.py` parity, `architecture.md` metrics row matches; hardware pending |
| FW-21 | E | firmware | `save_nvs` on every successful sensor apply, outside any lock | deferred | P3, audit §*Remaining firmware findings*; NVS write per slider event in the single-threaded control httpd — needs hardware to profile |
| FW-22 | E | firmware | `hdr_len` = unchecked `snprintf` return used as send length for `part_hdr[128]` | deferred | P3; currently maxes ~93 B so no truncation today |
| FW-23 | E | firmware | No PSRAM check before `fb_location = CAMERA_FB_IN_PSRAM` | deferred | P3; failure mode is a boot loop with no HTTP surface — needs hardware to observe |
| FW-24 | E | firmware | `client_slot(ip, true)` evicts by fewest fails (backwards policy) | deferred | P3; same policy as S-3, 4 slots |
| FW-25 | E | firmware | `strlen` after `strncpy(…,31)`; password checked `< 8` but not `> 63` | deferred | P3; config-path input validation, needs hardware |
| FW-26 | E | firmware | Discovery reply rate limit is one global `last_reply_us`, not per-source | deferred | P3; one client can suppress discovery for others |
| FW-27 | E | firmware | `atoi()` on caller-supplied strings in three places | deferred | P3; overflow is UB, impact low, no error channel |
| G-1 | A | desktop | §28 hardware capability detection entirely absent | open | Stage 5 (Phase 7 completion): capability probe + UI |
| G-2 | A | release | No licence/notice in the shipped package | open | Stage 5 (B5 packaging) |
| G-3 | A | hardware | ≥1 h soak gate never run (longest 293 s; one 145 s aborted) | deferred | **Phase 8 / D1** — the mandatory ≥60 min production-default soak, needs the camera on the softAP |
| G-4 | B | release | No installer (zips only) | open | Stage 5 (B5 packaging) |
| G-5 | B | release | `licensing.md` unclosed; dependency set understated | open | Stage 5 (B5 packaging) |
| G-6 | B | release | No CI — nothing ever builds or runs the tests | open | Stage 5 (C7): local `scripts/ci.ps1` only, no `.github` (AGENTS.md) |
| G-7 | B | release | No root README | open | Stage 5 (C7) |
| G-8 | B | release | `package-release.ps1` neither builds nor tests | open | Stage 5 (B5 packaging) |
| G-9 | B | release | Release script stamps the wrong commit as firmware provenance | open | Stage 5 (B5 packaging) |
| G-10 | B | release | Version split across four places, already inconsistent | open | Stage 5 (B5 packaging), with CP-24 |
| G-11 | B | release | `docs/deployment.md` stale for a shipped release | open | Stage 5 (B5 packaging) |
| G-12 | C | QML | Zero `Accessible.*` properties in the QML | open | Stage 5 |
| G-13 | C | tests | `tools/fake_camera.py` wired to nothing | open | Stage 5 (C7); partially addressed — the fake is now driven by `tools/config_contract_smoke.py` (g7/g8) |
| DD-1 | B | docs | Transport: ADR-0007/`protocol.md`/`architecture.md` said TCP primary | **closed** | Stage 4 g9: ADR-0007 status + amendment (*decided, not implemented*, measured basis preserved); `architecture.md` `transport` row and `protocol.md` status line say HTTP MJPEG ships |
| DD-2 | B | docs | Master Prompt §5 (router) contradicts ADR-0006 (softAP) | **closed** | Stage 4 g9: superseded banner on §5, original kept for provenance, one-network-per-camera consequence recorded |
| DD-3 | B | docs | Thread-model table claimed net-thread frame bus/recorder | **closed** | Stage 4 g9: verified **true** after CP-3 — `CameraDevice.cpp:184` uses `Qt::DirectConnection`, `FrameBus::setFrame` and `Recorder::appendFrame` run on the net thread |
| DD-4 | B | docs | Socket count said 4 → 7, source had 6 → 9 | **closed** | Stage 4 g6 (FW-17): corrected to 15 of 16 baseline, 18 with the transport |
| DD-5 | C | docs | `architecture.md:3` status line stuck at Phase 2 | **closed** | Stage 4 g9: rewritten to current phase with pointers to results and this tracker |
| DD-6 | C | docs | Module table names do not match source files | **closed** | Stage 4 g9: table now maps every module to its file (`camera.c`, `http_servers.c`, `frame_transport.c`, …) |
| DD-7 | C | docs | HD called "not the production default" after ADR-0010 chose it | **closed** | verified Stage 4 g9: `benchmark-results.md` §*Consequence* now says HD/q12 **is** the production default on a provisional floor, with ADR-0010 pointers |
| DD-8 | C | docs | `performance.md:3` still "numbers pending Phase 3/4" | **closed** | Stage 4 g9: status says targets vs measurements and points at `benchmark-results.md` |
| DD-9 | C | docs | `licensing.md` ESP-IDF "pin TBD" | **closed** | Stage 4 g9: pinned **v5.5.4**, 2026-09-23, ADR-0005, `scripts/install-idf.ps1` |
| DD-10 | C | docs | `testing.md` claims tests that do not exist | **closed** | Stage 4 g9: `tst_credentialstore` row lists its five real tests and names wrong-password/storage-unavailable as **not covered**; `tst_capture` row no longer claims "byte identity on the wire" |
| DD-11 | D | docs | SoftAP SSID missing from `protocol.md` | **closed** | Stage 4 g9: new *Device identity & discovery* section (SSID, IP, 48888, 2 s query, 250 ms reply, dedupe by id) |
| DD-12 | D | docs | "Verified against firmware 0.1.0" while release is 0.2.0 | **closed** | Stage 4 g9: note now says which build the observation came from and points at CP-24 |
| DD-13 | D | docs | Two byte-identical Master Prompt files | **closed** | Stage 4 g9: hyphen copy `git rm`'d (hashes matched after CRLF normalisation); AGENTS.md's em-dash file is the only one left |
| DD-14 | D | docs | `.gitignore` `*.dll`/`*.exe`/`*.map` unanchored | deferred | owner's own uncommitted `.gitignore` edit in the working tree — never staged from here; re-evaluate when that lands |
| S-1 | E | security | PBKDF2 at 8192 iterations (~2 orders below guidance) | deferred | Group E, P2 by the audit's own severity; softAP adversary is a joined client; a bump must move firmware and desktop together |
| S-2 | E | security | No timestamp/replay window on state-changing requests (`security.md:4` claims one) | deferred | Group E, P2; nonce single-use covers login only; accepted residual on a trusted LAN — the doc overclaims, revisit with S-1 |
| S-3 | E | security | `client_slot()` evicts by fewest fails | deferred | Group E, P3; same policy as FW-24, 4 slots |
| S-4 | E | security | `savePassword` never zeroes its plaintext copy | deferred | Group E, P3; defence-in-depth asymmetry, not a leak |
| S-5 | E | security | Video stream unauthenticated by design | **closed** | same finding as FW-18 — rationale and revisit triggers in `protocol.md` → *Stream authentication posture* |

---

## Gate log

| Gate | Date | Result | Evidence |
|---|---|---|---|
| Stage 1 test registration | 2026-09-29 | **pass** | fresh `desktop\build`: `ctest -N` = 12, `ctest` = 12/12, exit 0 |
| Stage 2 PSRAM reproducibility | 2026-09-29 | **pass** | clean generated `sdkconfig` identical before/after the fix; `40M` == `40M` |
| Stage 3 profile provenance | 2026-09-29 | **pass** | `ctest` 12/12; `tst_profileengine` 23/23; all 60 published figures present in `benchmark-results.md`; 3 mutations caught |
| Stage 4 g1 CP-3 thread placement | 2026-09-29 | **pass** | `ctest` 12/12 (4/4 repeat); `tst_capture` 9/9+1 skipped; `tst_recorder` 10/10; thread-placement mutation caught 15/15 |
| Stage 4 g1 CP-19 / FW-7 camera drain gate | 2026-09-30 | **pass (build + desktop only)** | `idf.py build` exit 0, no `camera.c` warnings; `ctest` 12/12; live hardware validation **not run** (Stage 4 g5) |
| Stage 4 g2 CP-6 401 replay | 2026-09-30 | **pass** | `ctest` 12/12 (4/4 repeat); `tst_devicestatus` 13/13; mutation of the 401 branch caught by both new tests (11/13, exit 2) |
| Stage 4 g3 CP-5 recovery budget | 2026-09-30 | **pass** | `ctest` 12/12, 62 s; `tst_capture` 10/0/1; 3-episode mutation caught (exit 1, 9/1/1) |
| Stage 4 g3 CP-18/FW-6 + CP-4/FW-1 | 2026-09-30 | **pass (build only)** | `idf.py build` exit 0, no `camera.c`/`app_main.c` warnings; live hardware validation **not run** (Stage 4 g5) |
| Stage 4 g4 CP-10/14/15/23/DX-1 | 2026-09-30 | **pass** | `ctest` 12/12 (61 s); `tst_notificationcenter` 15/15; `tst_sessionstate` 21/21; 3 mutations caught; `qmllint` differential 18 → 18 (no new); `SortingCamera -platform offscreen` loaded QML and ran 8 s clean |
| Stage 4 g5 CP-11/12/13/21/22 desktop seams | 2026-09-30 | **pass** | `ctest` **13/13**, 61.53 s (new suite `tst_configwritesequence`, 6/6); `tst_deviceregistry` 13/13, `tst_discovery` 17/0/2, `tst_sessionstate` 22/22, `tst_credentialstore` 8/8 (no longer writes HKCU); 5 mutations all caught |
| Stage 4 g6 FW-9/11/12/16/17/19/20 firmware truthfulness | 2026-09-30 | **pass (build + desktop only)** | `idf.py build` exit 0 after all seven edits; `ctest` **13/13**, 61.98 s; `tst_capture` 11/0/1; 1 mutation caught then restored green; **FW-9/11/12/16 need the camera, hardware validation not run** |
| Stage 4 g7 FW-13 / FW-10 POST-only config | 2026-09-30 | **pass (build + desktop + fake only)** | `idf.py build` exit 0 (split GET/POST handlers); `ctest` **13/13**, 62.08 s; 2 desktop mutations caught then restored green; fake-camera contract smoke **15/15** (script later committed as `tools/config_contract_smoke.py`); `py_compile` clean; **firmware refusal paths need the camera, hardware validation not run** |
| Stage 4 g8 CP-7/8/9 stream matrices, FW-18 rationale | 2026-09-30 | **pass (desktop + docs)** | `ctest` **13/13**, 84.37 s; `tst_capture` **16/0/1** (5 new slots, was 11); 4 mutations caught by their own test then restored green; `tools/config_contract_smoke.py` **18/18**; `idf.py build` not run (no firmware change); hardware not run |
| Stage 4 g9 FW-2/4/8/14, DX-12/17, CP-20 + documentation sweep | 2026-09-30 | **pass (build + desktop + docs)** | `idf.py build` exit 0 (bin 0xf80a0, no warnings); desktop full rebuild exit 0 (85 steps); `ctest` **13/13**, 83.15 s; `tst_capture` **17/0/1**, `tst_devicestatus` **14/0**; teardown measured **2 ms / 1 ms** against a 200 ms budget; all 42 untracked audit IDs dispositioned; **firmware findings stay `fixed` — the camera was never joined to the softAP** |
| Remediation gate | — | not run | — |
| Phase-7 acceptance | — | not run | — |
