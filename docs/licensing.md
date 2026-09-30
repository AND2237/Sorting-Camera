# Licensing / dependency report

_Commercial distribution target: app under proprietary terms; all linked/open-source deps must be redistribution-compatible. Update this file whenever a dependency is added — no dependency without a row here._

| Name | Version (planned/pinned) | License | Source | Why | Linking | Redistribution notes |
|---|---|---|---|---|---|---|
| Qt 6.11.2 (modules: Core, Gui, Qml, Quick, Network; Multimedia under evaluation) | 6.11.2 | LGPLv3 (modules chosen are LGPLv3; **GPLv3-only modules excluded**) | The Qt Company | UI + networking | Dynamic (default deploy) | Comply with LGPLv3: dynamic linking, ship notices, allow user relink/replace of Qt libs; no GPL-only Qt modules (HTTP Server, MQTT, CoAP, Quick3D, Graphs, Lottie, Virtual Keyboard, Wayland Compositor, Network Authorization, GRPC, OpenAPI-via-compiler caveats — re-verify per module before adding) |
| Qt tools (CMake/Ninja/MinGW kits) | CMake 3.30.5 / Ninja 1.12.1 / MinGW 13.1.0 | CMake: BSD-3; Ninja: Apache-2.0; MinGW-w64 runtime: mixed permissive (MIT/Zlib/BSD); libstdc++: GPL-3.0 **with GCC Runtime Library Exception** | Kitware / Ninja / MinGW-Builds via Qt installer | Build toolchain only | n/a (not shipped) | Toolchain not distributed with app; GCC exception permits linking app without copyleft |
| ESP-IDF | **v5.5.4** (pinned 2026-09-23 per ADR-0005; `scripts/install-idf.ps1` clones that tag) | Apache-2.0 | espressif/esp-idf | Firmware framework | Static into firmware | Ship notices; Apache-2.0 commercially fine |
| esp32-camera | 2.1.7 (latest stable at research date) | Apache-2.0 | espressif/esp32-camera | Official OV2640/JPEG driver | Static | As above |
| FreeRTOS / LwIP / ESP-IDF internals | bundled with IDF | MIT-style / BSD (see IDF licensing) | via IDF | OS + TCP/IP | Static | Notices via IDF |
| FFmpeg (desktop decode) | **not used** unless benchmark-justified | LGPL (configured) / GPL (full) | ffmpeg.org | JPEG decode fallback | n/a | LGPL build only if added; prefer Qt-native decode first |
| Fonts / icons / UI assets | system or self-created; any third-party must be OFL/CC0/BSD/MIT | — | — | UI | n/a | Avoid non-commercial or NC fonts |

## Qt licensing note (decision)

Open-source **LGPLv3** model selected (ADR-0003): dynamic linking, no static Qt into proprietary exe without compliance, keep Qt modules replaceable on deployed machines (standard `windeployqt` layout does this). If a required feature lands only in a GPL-only Qt module, re-evaluate before adding.

## Audit checklist before architecture lock

- [ ] Confirm each selected Qt module's license file in the installed `C:\Qt\6.11.2\mingw_64` (grep `license` in module mkspecs).
- [ ] Confirm ESP-IDF + esp32-camera pinned versions' LICENSE texts after clone.
- [ ] List any QML/JS/font assets and their licenses.
- [ ] Decide icon set (e.g. self-drawn or CC0) before Phase 7 UI polish.
