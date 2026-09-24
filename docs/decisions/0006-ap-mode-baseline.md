# ADR-0006: AP-mode baseline (ESP32 softAP, PC connects directly)

- **Status:** Accepted (2026-09-23, user-directed after live STA bring-up)
- **Context:** The STA+router baseline reached working L3 during Phase 2 bring-up (status HTTP 200 on `10.176.39.170`, RSSI −20 dBm, 0 disconnects). The user then directed a topology change: the ESP32 itself becomes the access point and the PC connects to it directly. This **deviates from Master Prompt §5** (both devices on the same factory router) — deviation approved by the user on 2026-09-23. Supersedes ADR-0001.
- **Decision:** ESP32 runs **softAP only** (no AP+STA coexistence for now):
  - SSID `ESP32-CAM`, WPA2-PSK, password provisioned via git-ignored `config_secrets.h` (`AP_SSID`/`AP_PASSWORD`), never logged
  - Fixed camera IP **`192.168.4.1`** (IDF softAP default); built-in DHCP server assigns the PC automatically
  - `max_connection` 4, channel 1, power save off
  - Desktop default host = `192.168.4.1`
  - AP-client RSSI reported in status JSON (`rssi`) via `esp_wifi_ap_get_sta_list()`

## Consequences

- The PC must join the camera's network → **no internet on the PC while connected** (single Wi-Fi radio).
- Fixed IP removes the need for DHCP-resilience/discovery in v1 — mDNS/UDP discovery deferred (see `architecture.md`); multi-camera scale-out later = one network per camera, or revisit hybrid AP+STA (future ADR).
- Benchmarks measure a **direct link** (no router in path): record AP channel + RSSI; the "router blip" stability test becomes PC-leave/rejoin + camera reboot.
- Deployment: operator connects the PC Wi-Fi to `ESP32-CAM` before launching the viewer.
- No router-based measurements existed before the switch, so no benchmark data was invalidated.

## Verification checklist

- [x] firmware builds; binary contains `ESP32-CAM`
- [x] flash + boot: serial shows `softAP "ESP32-CAM" up`
- [x] PC joins `ESP32-CAM`; status HTTP 200 at `http://192.168.4.1/api/v1/status`
- [x] snapshot + MJPEG stream render in the desktop app
- [x] camera frames contain JPEG SOI (fixed 2026-09-24: `camera_pins.h` D0–D5 were shifted; corrected to AI-Thinker map)

All items verified live 2026-09-24/25 (stream sustained, zero frame failures; see `benchmark-results.md`).
