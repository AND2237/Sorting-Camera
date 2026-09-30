# Sorting_Camera

ESP32-CAM (OV2640) → Wi-Fi → Windows viewer. The camera runs a **softAP**: it
broadcasts its own SSID (`ESP32-CAM`), the PC joins that network directly, and
the camera sits at a fixed `192.168.4.1`. Video is MJPEG over HTTP (port 81),
control is authenticated JSON over HTTP (port 80), discovery is a UDP announce on
48888. The desktop app is Qt 6.11 / C++17, LGPL modules only.

Priority order for every trade-off, everywhere: **image quality > sustained FPS
(min 15, preferred 20+) > latency**, with the live view staying usable.

## Status

Phase 7 (premium UI) completion. The Phase-6 audit is being worked through stage
by stage — what is fixed, what is build-verified only, what is deferred and who
owns it lives in [`docs/audits/PHASE7_REMEDIATION.md`](docs/audits/PHASE7_REMEDIATION.md).
The audit itself ([`GEMINI_PHASE6_AUDIT_2026-09-29.md`](docs/audits/GEMINI_PHASE6_AUDIT_2026-09-29.md))
is frozen and never modified.

Gates that are still open: the ≥1 h soak (D1) and the rest of Phase 8 need the
camera on the softAP; several firmware findings are `fixed` and stay `fixed`
until they have been proved on hardware.

## Prerequisites (verified 2026-09-23 on this machine)

| Tool | Version | Path |
|---|---|---|
| Qt | 6.11.2 `mingw_64` | `C:\Qt\6.11.2\mingw_64` |
| CMake | 3.30.5 | `C:\Qt\Tools\CMake_64\bin` |
| Ninja | 1.12.1 | `C:\Qt\Tools\Ninja` |
| MinGW | 13.1.0 | `C:\Qt\Tools\mingw1310_64\bin` |
| ESP-IDF | v5.5.4 | `%USERPROFILE%\esp\esp-idf` (`scripts/install-idf.ps1`) |
| Python | 3.x | for ESP-IDF and the tools under `tools/` |

Do **not** use MSYS2 g++ 16 for Qt builds — Qt 6.11.2 `mingw_64` needs the
matching MinGW 13.1.

## Build and test the desktop

```powershell
.\scripts\env.ps1
cmake -S desktop -B desktop\build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\mingw_64
cmake --build desktop\build
ctest --test-dir desktop\build --output-on-failure
```

15 suites; they must all pass. Three of them talk to real hardware only when
`SCAM_TEST_HOST` / `SCAM_TEST_PASSWORD` / `SCAM_DISCOVERY_PORT` are set and skip
otherwise, so a normal `ctest` run stays hermetic. Details:
[`docs/testing.md`](docs/testing.md).

Run it all in one step:

```powershell
.\scripts\ci.ps1              # configure (if needed) + build + ctest + contract smoke
.\scripts\ci.ps1 -SkipSmoke   # ... without the Python config-contract smoke
.\scripts\ci.ps1 -Firmware    # ... and build the ESP-IDF project
.\scripts\ci.ps1 -Package     # ... and, after a green run, package a release
```

The smoke step starts `tools/fake_camera.py` and drives the camera's config
contract (ADR-0017) without hardware, so it needs Python 3 and no Qt, no camera.

## Build the firmware

```powershell
.\scripts\idf-env.ps1
cd firmware\esp32_cam_stream
idf.py set-target esp32
idf.py build
```

The first build copies `config_secrets.example.h` to `config_secrets.h`, which is
git-ignored — set the AP SSID/WPA2 password and the device control password
there and never commit that file.

## Package a release

```powershell
.\scripts\package-release.ps1        # builds + tests first, then packages
.\scripts\package-release.ps1 -SkipBuild   # package an already green build
```

Produces `dist\SortingCamera-<version>-win64.zip` and
`dist\esp32_cam_stream-<version>-fw.zip`: binaries, flash script, README with
the `source commit`, and `THIRD_PARTY_NOTICES.txt` plus the full licence
texts under `licenses\`. The script refuses to package a red suite, a stale
exe, or an exe whose `--version` does not match the root `VERSION` file;
`.\scripts\ci.ps1 -Package` runs the same pipeline end to end.

## Use it

1. Flash the camera (firmware zip → `flash.ps1`), power-cycle it.
2. Join the `ESP32-CAM` Wi-Fi network from the PC — **the PC loses internet while
   connected.**
3. Start `SortingCamera.exe`; it discovers the device, signs in with the device
   control password, and connects to `192.168.4.1:81`.
4. F12 opens diagnostics, F11 fullscreen, Ctrl+Plus/Minus zoom, Ctrl+0 reset,
   Ctrl+T theme, Esc exits fullscreen.

Snapshots and recordings store the **exact received JPEG bytes** — never
decode → re-encode.

## Repository layout

```
firmware/    ESP-IDF project (esp32_cam_stream)
desktop/     Qt 6.11 application (scamcore library + Quick UI)
tests/       15 Qt Test suites, registered with CTest
docs/        architecture, protocol, benchmarks, security, deployment, licensing,
             testing, decisions/ (ADRs), audits/
tools/       fake camera, contract smoke, benchmark helpers
scripts/     env.ps1, idf-env.ps1, ci.ps1, package-release.ps1, install-idf.ps1
benchmarks/  benchmark harnesses and raw JSON results
```

## Documentation

| Document | What it answers |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | how the system is put together, threads, failure model |
| [`docs/protocol.md`](docs/protocol.md) | wire formats, control API, discovery, versioning |
| [`docs/testing.md`](docs/testing.md) | every suite, the live-device variables, mandatory §33 cases |
| [`docs/performance.md`](docs/performance.md) | targets (not measurements) |
| [`docs/benchmark-results.md`](docs/benchmark-results.md) | the measurements, each traceable to raw JSON in `benchmarks/results/` |
| [`docs/deployment.md`](docs/deployment.md) | build, package, flash, field update |
| [`docs/licensing.md`](docs/licensing.md) | dependency and licence report |
| [`docs/security.md`](docs/security.md) | threat model and controls |
| [`docs/decisions/`](docs/decisions) | ADRs 0001–0017 |
| [`AGENTS.md`](AGENTS.md) | the rules this codebase is built under |

## Licence

The application's licence is the project owner's to set; this repository does not
declare one yet — see [`docs/licensing.md`](docs/licensing.md). What *is*
declared are the third-party components and their licences (Qt LGPLv3 modules,
MinGW runtime, Mesa/D3Dcompiler, ESP-IDF, esp32-camera, …), recorded component
by component in `docs/licensing.md`. Shipping those notices alongside the
binaries (an LGPLv3 obligation, tracked as G-2) is done: both zips carry
`THIRD_PARTY_NOTICES.txt` and the full texts under `licenses\`.
