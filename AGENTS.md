# AGENTS.md — Sorting_Camera shared project instructions

## What this project is

ESP32-CAM (OV2640) → Wi-Fi → Windows Qt 6.11/C++ professional JPEG video viewer.
Full requirements: see `Master Prompt — ESP32-CAM to Qt Professional Low-Latency Video System.md` (repo root).

Priority order for every tradeoff: **Image quality > sustained FPS (min 15, preferred 20+) > latency**, while live view stays usable.

## Locked architectural decisions

- Network topology: **ESP32 softAP** (ADR-0006): the camera broadcasts its own SSID, the PC joins it directly, camera fixed IP `192.168.4.1`. Station+router mode (ADR-0001) is superseded.
- Firmware: **fresh ESP-IDF** (C/C++/CMake, official `espressif/esp32-camera` component). Arduino code elsewhere is reference only.
- Qt licensing: **LGPLv3-safe modules only** (Core, Gui, QML, Quick, Network, Multimedia if justified). Never add GPL-only Qt modules (HTTP Server, MQTT, CoAP, Quick3D, Graphs, Lottie, Virtual Keyboard…).
- Repository: everything lives here in `Sorting_Camera/`. Sibling `D:\Desktop\sorting-cam` is a frozen reference — never modify it, never treat it as source of truth.

## Source-of-truth order

1. actual hardware, 2. vendor docs, 3. SDK/API docs, 4. actual source, 5. our own reproducible benchmarks, 6. quality references, 7. assumptions.
Never invent benchmark numbers, FPS, latency, or hardware specs. Every measured claim needs: firmware+app version, config, PC hardware, date, duration.

## Repository layout

```
firmware/    ESP-IDF project(s)
desktop/     Qt 6.11 application
docs/        architecture, protocol, benchmark-plan/results, performance,
             security, deployment, licensing, testing, decisions/ (ADRs)
tools/       helper tooling
tests/       automated tests
benchmarks/  benchmark harnesses + raw result data
scripts/     environment / build / deploy scripts
```

Update docs whenever architecture or operational behavior changes. Record significant decisions as ADRs in `docs/decisions/`.

## Toolchain (verified 2026-09-23 on this machine)

Use `scripts\env.ps1` to put the correct tools on PATH for a session. **Do not use MSYS2 g++ 16 for Qt builds** — Qt 6.11.2 `mingw_64` requires the matching MinGW 13.1.

| Tool | Path |
|---|---|
| Qt 6.11.2 | `C:\Qt\6.11.2\mingw_64` |
| CMake 3.30.5 | `C:\Qt\Tools\CMake_64\bin` |
| Ninja 1.12.1 | `C:\Qt\Tools\Ninja` |
| MinGW 13.1.0 | `C:\Qt\Tools\mingw1310_64\bin` |
| ESP-IDF v5.5.4 | `C:\Users\Alireza\esp\esp-idf` (via `scripts\idf-env.ps1`) |

Desktop configure example:

```powershell
.\scripts\env.ps1
cmake -S desktop -B desktop\build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\mingw_64
cmake --build desktop\build
```

Firmware (after ESP-IDF env):

```powershell
.\scripts\idf-env.ps1
cd firmware\esp32_cam_stream
idf.py set-target esp32
idf.py build
```

## Engineering rules (do not violate)

- Measure before optimizing; benchmark before locking a transport/profile choice; every optimization gets baseline → change → measured result → ADR.
- No secrets in git: Wi-Fi SSID/password, device tokens → `config_secrets.h` (git-ignored) or NVS; commit only `config_secrets.example.h`.
- GUI thread never does network I/O or JPEG decode. QML = presentation only; C++ = networking/protocol/decode/record/metrics.
- Snapshot and recording store the **exact received JPEG bytes** — never decode→re-encode.
- Frames: latest-frame-wins, bounded buffers, no unbounded queues; every drop/corruption must be counted, never hidden.
- After every significant step: build, test, inspect logs, update docs. Keep the system buildable.
- Commit only when explicitly asked; focused commits; never commit build artifacts or credentials.
