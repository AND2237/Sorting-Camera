# Deployment

_Status: describes the artifacts the repository actually produces today
(`scripts\package-release.ps1`, version 0.2.0). Rewritten in Stage 5 of the
Phase-7 remediation — finding G-11 ("stale for a shipped release")._

## Artifacts

One command produces two zips; **there is no installer in v1** (see
*Installer* below — finding G-4).

| Artifact | Contents |
|---|---|
| `dist\SortingCamera-<version>-win64.zip` | `SortingCamera.exe`, the Qt 6.11 runtime in the standard `windeployqt` layout (plugins, `qml\` imports), the three MinGW runtime DLLs, `README.txt` (version + `source commit`), `THIRD_PARTY_NOTICES.txt`, `licenses\` (LGPLv3 + GPLv3 texts, GCC runtime exception, MinGW runtime texts, Qt's llvmpipe attribution) |
| `dist\esp32_cam_stream-<version>-fw.zip` | `esp32_cam_stream.bin`, `bootloader.bin`, `partition-table.bin`, a generated `flash.ps1`, `README.txt` (version + `source commit`), `THIRD_PARTY_NOTICES.txt`, `licenses\Apache-2.0.txt` |

## Build and package (Windows, this machine's toolchain)

```powershell
.\scripts\env.ps1                 # Qt 6.11.2 / CMake / Ninja / MinGW 13.1
.\scripts\ci.ps1                  # configure + build + version check + ctest + smoke
.\scripts\ci.ps1 -Firmware        # ... + the ESP-IDF firmware build
.\scripts\package-release.ps1     # builds + tests first (ci -Firmware), then packages
.\scripts\package-release.ps1 -SkipBuild   # package only (what ci.ps1 -Package does)
```

- The release script **fails** on a red suite, a stale exe, a missing version
  match between `VERSION` and the binary, or a missing/restricted Qt module
  (the GPL-module guard). It never packages first and tests later.
- **Version identity (G-10/CP-24):** the root `VERSION` file is the single
  source. It feeds the desktop `project(... VERSION ...)` (→ executable
  manifest, `SortingCamera --version`, `setApplicationVersion`), the firmware
  `PROJECT_VER`/`FW_VERSION`, and both zip names. The packager verifies the
  exe reports the version it is about to name the archive after.
- **Provenance (G-9):** both archives carry `source commit: <sha>` for the
  checkout the binaries were just built from, suffixed `-dirty` when the tree
  is not clean.
- **Notices (G-2):** `THIRD_PARTY_NOTICES.txt` + full licence texts under
  `licenses\` in both zips (see `docs/licensing.md`).

Desktop-only build/test without packaging: the commands in the root
`README.md` (`env.ps1`, `cmake`, `ctest`).

## Desktop deployment

- Portable zip: unpack anywhere on Windows 10+ x64, run `SortingCamera.exe`;
  no Qt or MinGW installation is required on the target PC (all DLLs ship).
- Config / user preferences: `QSettings` with org and application name
  `SortingCamera` — no secrets in settings; the device control password is
  handled by `CredentialStore`, which keeps it encrypted in OS-protected
  storage where the platform offers it and refuses to store it in clear text
  otherwise.
- Logs: `Diagnostics` installs a Qt message handler and keeps a rate-limited,
  five-level, per-subsystem ring **in memory**, shown in the diagnostics
  panel (F12). No log files are written; the handler chains to Qt's default, so
  the same messages go to stderr when the exe is run from a terminal. Tokens
  and passwords are never logged.
- Snapshots and recordings are written to a `recordings\` folder next to the
  executable and store the **exact received JPEG bytes**.
- Final HMI hardware unknown → no machine-specific assumptions; capability
  detection runs at startup (`Capabilities`, §28/G-1) and reports CPU,
  memory, graphics backend, acceleration, decode path, display, network
  interfaces and touch, with a fallback for each — see `docs/architecture.md`
  → *Hardware capability detection*.

## Firmware deployment

- Build (from a checkout):

  ```powershell
  .\scripts\idf-env.ps1
  cd firmware\esp32_cam_stream
  idf.py set-target esp32
  idf.py build
  ```

  The image version comes from the root `VERSION` file (`PROJECT_VER`), not
  from `git describe`. `scripts\ci.ps1 -Firmware` runs the same build from
  the repo root. **There is no `scripts/build-firmware.ps1`** — the old
  outline promised one; the commands above are the supported path.
- Flash: use the `flash.ps1` inside the firmware zip (board on USB), or
  `idf.py -p <PORT> flash` from a checkout. `flash.ps1` calls
  `esptool.py`/`python -m esptool`, which is an external tool
  (GPL-2.0-or-later, **not bundled** — `pip install esptool`).
- NVS holds the device's own AP credentials (SSID/WPA2 from the git-ignored
  `config_secrets.h`) + device password + last camera settings
  (schema-versioned, like the reference prototype's `SETTINGS_SCHEMA_VERSION`
  pattern).
- Network (ADR-0006): the device broadcasts the softAP `ESP32-CAM`, always at
  `192.168.4.1`; the operator connects the PC Wi-Fi to that SSID before using
  the viewer (the PC loses internet while connected).
- Per-unit identity: stable device ID derived from MAC/efuse-derived UID,
  independent of DHCP IP.

## Installer (G-4 — deferred)

v1 ships **portable zips** (app) and a **zip + `flash.ps1`** (firmware).
Building a Windows installer (Qt Installer Framework, MSIX, Inno Setup —
choice not made) is deferred until after the Phase-8 validation gates; it is
recorded as `deferred` with an owner in
`docs/audits/PHASE7_REMEDIATION.md` (G-4). Revisit when silent
install/upgrade, Start-menu entries, or embedded version/protocol metadata
in the installer become requirements.

## Field update story

v1: wired reflash (`flash.ps1` / `idf.py flash`). OTA explicitly out of
scope until a dual-OTA partition strategy is designed — do not add casually.
