# ADR-0005: ESP-IDF version pin

- **Status:** Accepted (2026-09-23) — clone, tool install, and full `esp32` baseline build verified on this machine. Hardware bring-up (last checklist item) still pending.
- **Context:** As of 2026-09, ESP-IDF stable = v6.0.x line; v5.5.x is a supported LTS-style branch (bugfix through ~Jan 2028 per Espressif support policy) and is the lower-risk match for classic ESP32 + `esp32-camera` 2.1.7. v6.0 has migration-breaking changes vs 5.x.
- **Decision:** Pin **ESP-IDF v5.5.4**. Move to v6.0 only if baseline build/tests show a concrete reason (new ADR then).

## Recorded facts (measured, 2026-09-23)

| Item | Value |
|---|---|
| Toolchain install date | 2026-09-23 |
| Clone location | `C:\Users\Alireza\esp\esp-idf` |
| Commit | `735507283d5b2f9fb363a1901172dbd9e847945d` (`git describe` = v5.5.4) |
| `idf.py --version` | ESP-IDF v5.5.4 |
| Target | esp32 (`idf.py set-target esp32`) |
| `espressif/esp32-camera` | **2.1.7** (lock: `firmware/esp32_cam_stream/dependencies.lock`) |
| `espressif/esp_jpeg` | **1.3.1** (same lock file) |
| Baseline build | `idf.py build` OK |
| App image | `esp32_cam_stream.bin` = 0xF12F0 bytes; app partition 0x200000 (53% free) |
| Bootloader | 0x6630 bytes (9% of its region free) |

Install/setup script: `scripts/install-idf.ps1`; session env: `scripts/idf-env.ps1`.

## Verification checklist

- [x] `idf.py --version` prints pinned tag (v5.5.4)
- [x] `idf.py set-target esp32` + full build succeeds
- [x] esp32-camera component version recorded in `dependencies.lock` + this ADR (2.1.7)
- [ ] OV2640 bring-up smoke test passes on hardware
