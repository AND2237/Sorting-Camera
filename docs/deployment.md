# Deployment

_Status: outline; finalize after Phase 7 packaging._

## Desktop (Windows)

- Build: Release, Ninja, MinGW 13.1 + Qt 6.11.2 (see `scripts/env.ps1`).
- Package: CMake `qt_generate_deploy_app_script()` / `windeployqt` to collect Qt runtime (Core, Gui, Qml, Quick, Network, Multimedia if used) + MinGW runtime DLLs.
- Config/user prefs: `QSettings` (org = product name) — no secrets in settings; device control password entered at runtime, cached only in OS-appropriate protected storage if at all.
- Logs: rotating file under `%LOCALAPPDATA%\<app>\logs`, level from settings; never log tokens/passwords.
- Installer: to be selected (Qt Installer Framework vs alternative) after app stabilizes; include version + protocol compatibility metadata.
- Final HMI hardware unknown → no machine-specific assumptions; capability detection runs at startup (`Capabilities`, §28/G-1) and reports CPU, memory, graphics backend, acceleration, decode path, display, network interfaces and touch, with a fallback for each — see `docs/architecture.md` → *Hardware capability detection*.

## Firmware

- Flash via `idf.py flash`; NVS holds the device's own AP credentials (SSID/WPA2 from `config_secrets.h`) + device password + last camera settings (schema-versioned, like the reference prototype's `SETTINGS_SCHEMA_VERSION` pattern).
- Network (ADR-0006): device broadcasts softAP `ESP32-CAM`, always at `192.168.4.1`; the operator connects the PC Wi-Fi to that SSID before using the viewer (PC loses internet while connected).
- Per-unit identity: stable device ID derived from MAC/efuse-derived UID, independent of DHCP IP.
- Provide `scripts/build-firmware.ps1` + serial-monitor helper once the project builds.

## Field update story

v1: wired reflash. OTA explicitly out of scope until partition strategy (dual OTA) is designed — do not add casually.
