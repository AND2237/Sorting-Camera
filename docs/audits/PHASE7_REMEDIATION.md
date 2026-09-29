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

## Finding status

| ID | Sev | Area | Summary | Status | Evidence |
|---|---|---|---|---|---|
| CP-1 | A | desktop | Test suites never registered | closed | Stage 1: `ctest -N` 12, `ctest` 12/12 |
| CP-2 | A | desktop | `ProfileEngine` borrows another profile's byte count | open | — |
| CP-3 | A | desktop | Recording and frame bus run on the GUI thread | open | — |
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
| CP-16 | B | desktop | 7 fps floor presented as measured | open | — |
| CP-17 | B | desktop | D2 evidence string overstates the run | open | — |
| CP-18 | B | firmware | NVS restore trusts types with no validation | open | — |
| CP-19 | A | firmware | Frame mutex held across network send | open | — |
| CP-21 | C | desktop | Protocol version check absent | open | — |
| CP-22 | C | desktop | Test settings path not redirected | open | — |
| CP-23 | C | QML | Decode-failure counter labelled as transport drops | open | — |
| CP-25 | B | desktop | Conservative reading rule documented but not implemented | open | — |
| DX-1 | C | QML | Severity conflated for sign-in failure | open | — |
| DX-12 | D | desktop | `FrameImageProvider` outside `scamcore` | open | — |
| DX-17 | D | desktop | `~CameraDevice` blocking-queued across threads | open | — |
| A0 | A | firmware | PSRAM config unreproducible | **fixed pending** | Stage 0 proves reproducible; line fix + ADR |
| FW-1 | A | firmware | Control reset on re-init (alias of CP-4) | open | — |
| FW-2 | C | firmware | Discovery accepts oversized query | open | — |
| FW-3 | A | firmware | PSRAM config divergence | **verified** | Stage 0 table above — no divergence exists |
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
| Remediation gate | — | not run | — |
| Phase-7 acceptance | — | not run | — |
