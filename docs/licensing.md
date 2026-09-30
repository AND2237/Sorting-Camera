# Licensing / dependency report

_Commercial distribution target: app under proprietary terms; all linked/open-source deps must be redistribution-compatible. Update this file whenever a dependency is added — no dependency without a row here._

| Name | Version (planned/pinned) | License | Source | Why | Linking | Redistribution notes |
|---|---|---|---|---|---|---|
| Qt 6.11.2 (modules: Core, Gui, Qml, Quick, Network; Multimedia under evaluation) | 6.11.2 | LGPL-3.0-only (Qt is offered commercial **or** LGPL-3.0-only **or** GPL-2.0-only **or** GPL-3.0-only; we deploy the LGPL-3.0-only option; **GPLv3-only modules excluded**) | The Qt Company | UI + networking | Dynamic (default deploy) | Comply with LGPL-3.0: dynamic linking, ship notices and the license texts, allow user relink/replace of Qt libs; no GPL-only Qt modules (HTTP Server, MQTT, COAP, Quick3D, Graphs, Lottie, Virtual Keyboard, Wayland Compositor, Network Authorization, GRPC, OpenAPI-via-compiler caveats — re-verify per module before adding) |
| Qt plugins shipped by `windeployqt` (`platforms\qwindows.dll`, `imageformats\q{gif,ico,jpeg,svg}.dll`, `tls\*`, `iconengines`, `generic`, `networkinformation`, `platforminputcontexts`, `vectorimageformats`, plus the `qml\` imports) | 6.11.2 | LGPL-3.0-only | The Qt Company | runtime plugins | Dynamic (DLLs next to the exe) | Part of the Qt row; same LGPLv3 obligations, satisfied by shipping them unmodified and replaceable |
| MinGW runtime DLLs **shipped with the app**: `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` | 13.1.0 | libgcc/libstdc++: GPL-3.0 **with GCC Runtime Library Exception**; winpthreads: permissive (MIT/BSD) — texts in `third_party\licenses\` | MinGW-Builds via Qt installer | C/C++ runtime for the exe | Dynamic | Exception permits linking the proprietary app without copyleft; ship the exception + runtime texts (release script stages them) |
| Qt build tools (CMake/Ninja/MinGW kits) | CMake 3.30.5 / Ninja 1.12.1 / MinGW 13.1.0 | CMake: BSD-3; Ninja: Apache-2.0; MinGW-w64 tools: mixed permissive | Kitware / Ninja / MinGW-Builds via Qt installer | Build toolchain only | n/a (not shipped) | Toolchain not distributed with the app; only the three runtime DLLs above ship |
| Mesa llvmpipe software renderer (`opengl32sw.dll`) | ships with Qt 6.11.2 binaries | **MIT and Boost Software License 1.0** (per Qt attribution `qt-attribution-llvmpipe.html`) | Mesa3D (via Qt package) | OpenGL software fallback | Dynamic | Text shipped in the app archive (`licenses\mesa-llvmpipe-attribution.html`) |
| `D3Dcompiler_47.dll` | 6.3.9600.16384 | Microsoft "Direct3D HLSL Compiler for Redistribution" — not open source | Microsoft (copied by `windeployqt`) | HLSL compiler DLL pulled in by Qt | Dynamic | Redistributed under Microsoft's terms for this binary; listed in `THIRD_PARTY_NOTICES.txt` |
| esptool (flashing tool used by `flash.ps1`) | 4.12.0 (ESP-IDF Python env) | **GPL-2.0-or-later** | espressif/esptool | flash the three firmware images | n/a (**not shipped**) | External tool the operator installs (`pip install esptool`); no esptool code is redistributed, so the GPL is not triggered by our archives |
| ESP-IDF | **v5.5.4** (pinned 2026-09-23 per ADR-0005; `scripts/install-idf.ps1` clones that tag) | Apache-2.0 | espressif/esp-idf | Firmware framework | Static into firmware | Ship notices; Apache-2.0 commercially fine |
| esp32-camera | 2.1.7 (latest stable at research date) | Apache-2.0 | espressif/esp32-camera | Official OV2640/JPEG driver | Static | As above |
| FreeRTOS / LwIP / ESP-IDF internals | bundled with IDF | MIT-style / BSD (see IDF licensing) | via IDF | OS + TCP/IP | Static | Notices via IDF |
| FFmpeg (desktop decode) | **not used** unless benchmark-justified | LGPL (configured) / GPL (full) | ffmpeg.org | JPEG decode fallback | n/a | LGPL build only if added; prefer Qt-native decode first |
| Fonts / icons / UI assets | **none bundled** — confirmed: `desktop\qml\` contains only `Main.qml` and `Theme.qml`, no font/image files, no `source:` references | n/a (system fonts, shapes drawn by QML) | self-created | UI | n/a | If any third-party asset is ever added it must be OFL/CC0/BSD/MIT; no NC fonts |

## Qt licensing note (decision)

Open-source **LGPLv3** model selected (ADR-0003): dynamic linking, no static Qt into proprietary exe without compliance, keep Qt modules replaceable on deployed machines (standard `windeployqt` layout does this). If a required feature lands only in a GPL-only Qt module, re-evaluate before adding.

## What ships where (G-2)

`scripts\package-release.ps1` stages `THIRD_PARTY_NOTICES.txt` into **both**
archives plus a `licenses\` folder with the full texts:

- app zip: `LGPL-3.0.txt`, `GPL-3.0.txt` (LGPLv3 is additional terms on top of
  GPLv3, so both are needed), `GCC-RUNTIME-LIBRARY-EXCEPTION.txt`,
  `mingw-w64-runtime.txt`, `winpthreads.txt`,
  `mesa-llvmpipe-attribution.html`
- firmware zip: `Apache-2.0.txt` (ESP-IDF and esp32-camera)

The texts live in `third_party\licenses\` (canonical gnu.org / apache.org
downloads, the GCC runtime exception and MinGW runtime texts from the pinned
MinGW 13.1.0 install, and Qt's own llvmpipe attribution page).

## Audit checklist before architecture lock

- [x] **Qt module licenses confirmed** — the installed Qt ships SBOMs: each of
  `qtbase`, `qtdeclarative`, `qtsvg` (the sources of the shipped DLLs) lists
  its packages as `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only
  OR GPL-3.0-only` (`C:\Qt\6.11.2\mingw_64\sbom\*.spdx.json`), and the
  installer recorded `License type [Opensource]` (`C:\Qt\licenseInfo.txt`).
  No module mkspecs carry a license field — the SBOMs are the source of
  truth here.
- [x] **ESP-IDF + esp32-camera LICENSE texts confirmed after clone** —
  `C:\Users\Alireza\esp\esp-idf\LICENSE` and
  `firmware\esp32_cam_stream\managed_components\espressif__esp32-camera\LICENSE`,
  both the Apache License 2.0 text.
- [x] **QML/JS/font assets listed** — none: `desktop\qml\` holds only
  `Main.qml` and `Theme.qml`, with no font/image files and no `source:`
  references; all visuals are QML-drawn shapes and system fonts.
- [x] **Icon set decision recorded** — no icon assets are used (and none
  ship); if icons are added later they must be self-drawn, OFL, CC0, BSD or
  MIT, and get a row here before release.
