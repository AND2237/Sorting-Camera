# Remediation tracker — Phase 6 audit (2026-09-29)

Working record for closing `GEMINI_PHASE6_AUDIT_2026-09-29.md`. Every row carries the
evidence that closed it: a command and its output, a test name, or a measurement.

The audit file itself is a frozen record and is never modified.

Status key: `open` → `fixed` (code changed, build green) → `verified` (regression test or
measurement proves the fix) → `closed` (gate evidence complete).

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
| CP-7 | B | tests | No chunked-transfer test | open | — |
| CP-8 | B | tests | No reconnect-ladder test | open | — |
| CP-9 | B | tests | No malformed-frame matrix | open | — |
| CP-10 | C | QML | Overlapping anchored labels | **closed** | Stage 4 g4: three labels wrapped in a `Column`; qmllint differential clean, runtime load clean |
| CP-11 | C | desktop | `releaseStale` blocks the GUI thread | open | — |
| CP-12 | C | desktop | Skip ahead passes no format | open | — |
| CP-13 | C | desktop | Frame interval metrics throttled incorrectly | open | — |
| CP-14 | C | QML | Slider binding reads `parent.current` | **closed** | Stage 4 g4: `syncFromStatus()` + `onCurrentChanged`, `!pressed` guarded; qmllint differential clean |
| CP-15 | C | QML | Dismisses by index, not by identity | **closed** | Stage 4 g4: `ItemIdRole` + `dismissById`, `dismiss(int)` deleted; new test, 3 mutations caught |
| CP-16 | B | desktop | 7 fps floor presented as measured | **closed** | Stage 3: `floorProvisional` + `activeFloorProvisional`; stale doc section replaced |
| CP-17 | B | desktop | D2 evidence string overstates the run | **closed** | Stage 3: alt 9.97, 28,956 B, "one run measured three ways"; ADR-0012 corrected |
| CP-18 | B | firmware | NVS restore trusts types with no validation | **fixed** | Stage 4 g3: enum + floor/ceiling checks in `camera_cfg_restore()`; build-verified, hardware pending g5 |
| CP-19 | A | firmware | Frame mutex held across network send | **fixed** | Stage 4 g1: drain gate in `camera.c` (ADR-0016); `idf.py build` exit 0; hardware validation pending (Stage 4 g5) |
| CP-21 | C | desktop | Protocol version check absent | open | — |
| CP-22 | C | desktop | Test settings path not redirected | open | — |
| CP-23 | C | QML | Decode-failure counter labelled as transport drops | **closed** | Stage 4 g4: `dec` and `drop` shown separately with tooltips, no sum |
| CP-25 | B | desktop | Conservative reading rule documented but not implemented | **closed** | Stage 3: `conservativeFps()` used by `recommended()` and `activeMeetsFloor()`; disagreement forced by test |
| DX-1 | C | QML | Severity conflated for sign-in failure | **closed** | Stage 4 g4: `SessionState::notificationLevel()` consulted by `CameraDevice`; new test, mutation caught |
| DX-12 | D | desktop | `FrameImageProvider` outside `scamcore` | open | — |
| DX-17 | D | desktop | `~CameraDevice` blocking-queued across threads | open | — |
| A0 | A | firmware | PSRAM config unreproducible | **closed** | ADR-0015: clean config == measured config; generated `sdkconfig` diff IDENTICAL |
| FW-1 | A | firmware | Control reset on re-init (alias of CP-4) | **fixed** | Stage 4 g3 (CP-4); build-verified, hardware pending g5 |
| FW-2 | C | firmware | Discovery accepts oversized query | open | — |
| FW-3 | A | firmware | PSRAM config divergence | **closed** | No divergence exists — see ADR-0015; `80M` was silently discarded, `40M` measured and produced |
| FW-4 | B | firmware | Unbounded recovery on stream failure | open | — |
| FW-6 | B | firmware | NVS restore unvalidated (alias of CP-18) | **fixed** | Stage 4 g3 (CP-18); build-verified, hardware pending g5 |
| FW-7 | A | firmware | Camera lock held across send (alias of CP-19) | **fixed** | Stage 4 g1 part 2: same change as CP-19; see ADR-0016 |
| FW-8 | B | firmware | Watchdog resets camera at 1 Hz | open | — |
| FW-9 | C | firmware | Missing close delimiter | open | — |
| FW-10 | B | firmware | Truncated config query returns 200 OK | open | — |
| FW-11 | C | firmware | Quality-blind frame estimate | open | — |
| FW-12 | C | firmware | Sensor endpoint lacks lock and debounce | open | — |
| FW-13 | B | firmware | Config accepts GET, spec says POST | open | — |
| FW-16 | C | firmware | Path cited for secrets check is wrong | open | — |
| FW-17 | C | firmware | Socket budget in three places, three answers | open | — |
| FW-19 | D | firmware | Counter definition undocumented | open | — |
| FW-20 | D | firmware | Documented counter not implemented | open | — |

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
| Remediation gate | — | not run | — |
| Phase-7 acceptance | — | not run | — |
