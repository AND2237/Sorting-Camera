# Phase 1 — build all six variants (ADR-0018 trial, unflashed)

_Date: 2026-10-06; rebuilt 2026-10-08 after the info-endpoint change, 2026-10-10
after the sensor-defaults fix. No hardware.
Nothing below was flashed; no fps/throughput/current numbers are known. This file
records that the six firmware variants configure and link._

- Firmware HEAD: `80fcff8` (`80fcff87f12a9eae49758ff3b91748510f5f2355`, "New workplan setup")
  plus working tree from `patches/incremental-on-80fcff8.patch`
  (SHA-256 `ca64c63bd4c7823425203e91038020e3e173e23640d0aedd66b73ab55d463372`)
   and `patches/info-endpoint-on-incremental.patch`
   (SHA-256 `8d94e716255d60dee3c6a230ac52240a0c2a729c56ab4411832786201cb870d6`)
   and `patches/sensor-defaults-fix.patch`
   (SHA-256 `f33e608b47c71edda2185826e6d09b9690a2a78d4e4f37deca591c33cd772edc`).
   The info-endpoint change adds the `INFO` request to the bench firmware
   (`main/net_bench.c`, `spi_flash` requirement) and `--info-only` / run
   cross-checks to `benchmarks/net_bench.py`; no `sdkconfig.*.defaults` changed,
   so every variant kept its Phase 1 configuration.
   The sensor-defaults fix (ADR-0019) makes `camera_control_init` start from the
   documented sensor defaults when no NVS record exists (previously all zeros,
   which switched auto exposure/gain off and streamed black frames);
   `load_nvs` now reports whether a valid record was found. It also adds
   `benchmarks/sensor_tool.py`. No `sdkconfig.*.defaults` changed, so every
   variant kept its configuration.
- ESP-IDF: v5.5.4. Toolchain: Xtensa GCC 14.2.0 (`esp-14.2.0_20260121`).
  Components: `esp32-camera` v2.1.7, `esp_jpeg` 1.3.1 (from lock/managed components).
- Target: `esp32`. Each variant has its own build dir and generated `sdkconfig`
  (ADR-0015 rule: never inherit). Commands from `firmware/esp32_cam_stream`
  after `scripts\idf-env.ps1`:

```powershell
idf.py -B build-p   -D SDKCONFIG=build-p/sdkconfig   -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults"                                        set-target esp32; idf.py -B build-p   build
idf.py -B build-pb  -D SDKCONFIG=build-pb/sdkconfig  -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults"                               set-target esp32; idf.py -B build-pb  build
idf.py -B build-nb  -D SDKCONFIG=build-nb/sdkconfig  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench-nocam.defaults"          set-target esp32; idf.py -B build-nb  build
idf.py -B build-nbb -D SDKCONFIG=build-nbb/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults;sdkconfig.bench-nocam.defaults" set-target esp32; idf.py -B build-nbb build
idf.py -B build-nc  -D SDKCONFIG=build-nc/sdkconfig  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench-cam.defaults"            set-target esp32; idf.py -B build-nc  build
idf.py -B build-ncb -D SDKCONFIG=build-ncb/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults;sdkconfig.bench-cam.defaults"  set-target esp32; idf.py -B build-ncb build
```

Variant key: P = production/new block, PB = production/baseline,
NB = net-bench no-camera/new, NBB = net-bench no-camera/baseline,
NC = net-bench with-camera/new, NCB = net-bench with-camera/baseline.

## 1. Build result + memory (`idf.py size`, `.bin` file length)

| Variant | Defaults | Build | `esp32_cam_stream.bin` | IRAM used / free (of 131072) | DRAM used / free (of 180736) | Warnings (`warning:` in full log) |
|---|---|---|---|---|---|---|
| P (build-p) | `sdkconfig.defaults` (new block) | links | 1021184 B (`0xf9500`) | 121147 / 9925 (92.43%) | 48216 / 132520 (26.68%) | 0 |
| PB (build-pb) | `sdkconfig.baseline.defaults` | links | 1017680 B (`0xf8750`) | 118431 / 12641 (90.36%) | 48216 / 132520 (26.68%) | 0 |
| NB (build-nb) | `sdkconfig.defaults` + `sdkconfig.bench-nocam.defaults` | links | 1023568 B (`0xf9e50`) | 120879 / 10193 (92.22%) | 50096 / 130640 (27.72%) | 0 |
| NBB (build-nbb) | `sdkconfig.baseline.defaults` + `sdkconfig.bench-nocam.defaults` | links | 1020064 B (`0xf90a0`) | 118163 / 12909 (90.15%) | 50096 / 130640 (27.72%) | 0 |
| NC (build-nc) | `sdkconfig.defaults` + `sdkconfig.bench-cam.defaults` | links | 1024608 B (`0xfa260`) | 120879 / 10193 (92.22%) | 50096 / 130640 (27.72%) | 0 |
| NCB (build-ncb) | `sdkconfig.baseline.defaults` + `sdkconfig.bench-cam.defaults` | links | 1021120 B (`0xf94c0`) | 118163 / 12909 (90.15%) | 50096 / 130640 (27.72%) | 0 |

Notes:

- Deltas (new − baseline): P−PB = +2716 IRAM, +3504 `.bin` B;
  NB−NBB = +2716 IRAM, +3504 `.bin` B; NC−NCB = +2716 IRAM, +3488 `.bin` B.
  Matches the ADR-0018 evidence magnitude (new block ≈ +2.7 KB IRAM vs baseline).
- Rebuild vs 2026-10-08 (info-endpoint state): IRAM byte-identical in all six
  variants (P 121147, PB 118431, NB/NC 120879, NBB/NCB 118163 — as expected:
  the sensor-defaults change adds no IRAM). DRAM unchanged in all six.
  All `.bin` grew by +352..+384 B (P +368, PB/NB/NBB/NCB +352, NC +384).
- Rebuild vs Phase 1 (info-endpoint added): IRAM byte-identical in all six
  variants (P 121147, PB 118431, NB/NC 120879, NBB/NCB 118163 — as expected).
  P/PB `.bin` and DRAM unchanged (INFO code is compiled out without
  `CONFIG_SORTING_CAM_NET_BENCH`). Bench variants grew: `.bin` +2064..+2176 B
  (NB +2160, NBB +2144, NC +2064, NCB +2176) and DRAM 48208 → 50096 (+1888 B:
  INFO last-run state and reply buffer).
- `idf.py size` "Total image size": P 1021072, PB 1017564, NB 1023452,
  NBB 1019952, NC 1024484, NCB 1021008 bytes (`.bin` padded larger).
- No `warning:` (case-insensitive: no `warning` at all) in any of the six full build logs.
  `main/*.c` objects built include `camera.c`, `camera_control.c`, `wifi.c`,
  `app_main.c`, `net_bench.c`, `auth.c`, `metrics.c`, `frame_transport.c`,
  `http_servers.c`, `discovery.c`.
- Smallest app partition is `0x200000` bytes; all six `.bin` leave ~51% free.
  Bootloader `0x6ca0` bytes, `0x360` bytes (3%) free (build-p; others identical class).

## 2. Generated-`sdkconfig` verification (read back, never from the defaults file)

| Symbol (generated `build-*/sdkconfig`) | P | PB | NB | NBB | NC | NCB |
|---|---|---|---|---|---|---|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 240 | 160 | 240 | 160 | 240 | 160 |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` / `_DIO` | QIO=y / DIO n | DIO=y | QIO=y / DIO n | DIO=y | QIO=y / DIO n | DIO=y |
| `CONFIG_ESPTOOLPY_FLASHMODE` (string) | `"dio"` (see §3) | `"dio"` | `"dio"` (see §3) | `"dio"` | `"dio"` (see §3) | `"dio"` |
| `CONFIG_ESPTOOLPY_FLASHFREQ` | `"80m"` | `"40m"` | `"80m"` | `"40m"` | `"80m"` | `"40m"` |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 32768 | 5760 | 32768 | 5760 | 32768 | 5760 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 32768 | 5760 | 32768 | 5760 | 32768 | 5760 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 64 | 32 | 64 | 32 | 64 | 32 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 64 | 32 | 64 | 32 | 64 | 32 |
| `CONFIG_LWIP_IRAM_OPTIMIZATION` | y | not set | y | not set | y | not set |
| `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` | y | not set | y | not set | y | not set |
| `CONFIG_ESP_WIFI_IRAM_OPT` | y | y | y | y | y | y |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | y | y | y | y | y | y |
| `CONFIG_SPIRAM_SPEED_40M` / `_80M` | 40M=y | 40M=y | 40M=y | 40M=y | 40M=y | 40M=y |
| `CONFIG_SORTING_CAM_NET_BENCH` | not set | not set | y | y | y | y |
| `CONFIG_SORTING_CAM_NET_BENCH_NO_CAMERA` | not set | not set | y | y | not set | not set |

Expectation from `docs/performance-workplan.md` §1.1 holds for CPU (`240` vs `160`),
flash frequency (`"80m"` vs `"40m"`), TCP windows (`32768` vs `5760`),
`LWIP_IRAM_OPTIMIZATION` (`y` vs not set), and both bench flags.

## 3. Flash mode

In all three new-block variants the generated `sdkconfig` contains the combination:

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHMODE="dio"
```

and `build-*/flash_args` line 1 plus the `idf.py build` flash hint both read
`--flash_mode dio --flash_freq 80m` (baseline variants: `dio` / `40m`).

Flash mode: CONFIG_ESPTOOLPY_FLASHMODE="dio" with CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
is expected ESP-IDF behaviour, not a defect. The image header stays DIO so the ROM
can boot; on ESP32 the second-stage bootloader then enables QIO and the app flash
driver uses it. The 80m frequency is written into the header. Not yet verified on
hardware: the INFO field runtime.flash_io must read qio for new-block builds and
dio for baseline builds.

## 4. Not done (needs hardware)

- `idf.py -B <dir> -p COMPx flash monitor`, §1.1 --info-only checks after flashing (camera_enabled, build fields, runtime.flash_io qio/dio, radio fields).
- Phase 2 link matrix (`benchmarks/net_bench.py` int/psram/cam).
- Gate G1 table in `docs/benchmark-results.md` and ADR-0018 kept/reverted verdict.
