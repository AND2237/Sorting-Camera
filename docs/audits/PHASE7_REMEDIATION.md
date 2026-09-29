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

### Still open in g1

| ID | Why |
|---|---|
| CP-19 / FW-7 | firmware holds the frame mutex across the network send — next in this group |

### Gate

| Check | Result |
|---|---|
| build | exit 0 |
| `ctest --output-on-failure` | **100% tests passed, 0 failed out of 12**, 16.57 s, exit 0 |
| repeat `ctest` | 4/4 rounds green |
| `tst_recorder` | **Totals: 10 passed, 0 failed** |
| `tst_capture` | **Totals: 9 passed, 0 failed, 1 skipped** (live test, needs `SCAM_TEST_HOST`) |

---

## Finding status

| ID | Sev | Area | Summary | Status | Evidence |
|---|---|---|---|---|---|
| CP-1 | A | desktop | Test suites never registered | closed | Stage 1: `ctest -N` 12, `ctest` 12/12 |
| CP-2 | A | desktop | `ProfileEngine` borrows another profile's byte count | **closed** | Stage 3: 16982 (own config), 14680 rejected by test; mutation-tested |
| CP-3 | A | desktop | Recording and frame bus run on the GUI thread | **closed** | Stage 4 g1: Direct frame hop + `DirectConnection` delivery + synchronised `Recorder`; mutation-caught |
| CP-4 | A | firmware | Sensor controls reset on every `esp_camera_init` | open | — |
| CP-5 | A | desktop | Recovery counter never resets | open | — |
| CP-6 | A | desktop | 401 during connect permanently dead | open | — |
| CP-7 | B | tests | No chunked-transfer test | open | — |
| CP-8 | B | tests | No reconnect-ladder test | open | — |
| CP-9 | B | tests | No malformed-frame matrix | open | — |
| CP-10 | C | QML | Overlapping anchored labels | open | — |
| CP-11 | C | desktop | `releaseStale` blocks the GUI thread | open | — |
| CP-12 | C | desktop | Skip ahead passes no format | open | — |
| CP-13 | C | desktop | Frame interval metrics throttled incorrectly | open | — |
| CP-14 | C | QML | Slider binding reads `parent.current` | open | — |
| CP-15 | C | QML | Dismisses by index, not by identity | open | — |
| CP-16 | B | desktop | 7 fps floor presented as measured | **closed** | Stage 3: `floorProvisional` + `activeFloorProvisional`; stale doc section replaced |
| CP-17 | B | desktop | D2 evidence string overstates the run | **closed** | Stage 3: alt 9.97, 28,956 B, "one run measured three ways"; ADR-0012 corrected |
| CP-18 | B | firmware | NVS restore trusts types with no validation | open | — |
| CP-19 | A | firmware | Frame mutex held across network send | open | — |
| CP-21 | C | desktop | Protocol version check absent | open | — |
| CP-22 | C | desktop | Test settings path not redirected | open | — |
| CP-23 | C | QML | Decode-failure counter labelled as transport drops | open | — |
| CP-25 | B | desktop | Conservative reading rule documented but not implemented | **closed** | Stage 3: `conservativeFps()` used by `recommended()` and `activeMeetsFloor()`; disagreement forced by test |
| DX-1 | C | QML | Severity conflated for sign-in failure | open | — |
| DX-12 | D | desktop | `FrameImageProvider` outside `scamcore` | open | — |
| DX-17 | D | desktop | `~CameraDevice` blocking-queued across threads | open | — |
| A0 | A | firmware | PSRAM config unreproducible | **closed** | ADR-0015: clean config == measured config; generated `sdkconfig` diff IDENTICAL |
| FW-1 | A | firmware | Control reset on re-init (alias of CP-4) | open | — |
| FW-2 | C | firmware | Discovery accepts oversized query | open | — |
| FW-3 | A | firmware | PSRAM config divergence | **closed** | No divergence exists — see ADR-0015; `80M` was silently discarded, `40M` measured and produced |
| FW-4 | B | firmware | Unbounded recovery on stream failure | open | — |
| FW-6 | B | firmware | NVS restore unvalidated (alias of CP-18) | open | — |
| FW-7 | A | firmware | Camera lock held across send (alias of CP-19) | open | — |
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
| Remediation gate | — | not run | — |
| Phase-7 acceptance | — | not run | — |
