# ADR-0018: Wi-Fi / TCP throughput tuning block in `sdkconfig.defaults` (trial; link measured, fps not yet measured)

_Status: trial; Phase 2 link test done (camera off): kept provisionally pending the camera-on
A/B (NC vs NCB); RAM headroom under load is the open risk. RAM headroom measured only in the
bench build; not yet in the production build._

_No result in this document is a measurement of the camera._

_Correction, same day: the first version of this ADR claimed the block was verified off-target. It
had only been run through a Kconfig generator, never built. A real build (ESP-IDF v5.5.4, all four
variants listed under Evidence) showed that `LWIP_IRAM_OPTIMIZATION` alone does not link on this
firmware (IRAM overflow, 440 bytes). The block now also sets
`FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`, and that was built._

## Context

The project owner asked for a set of ESP32 Wi-Fi/TCP throughput settings (240 MHz CPU,
QIO flash at 80 MHz, larger lwIP TCP windows, more Wi-Fi dynamic buffers, IRAM placement of
the hot Wi-Fi/TCP paths, Wi-Fi memory kept in internal RAM) to be applied to
`firmware/esp32_cam_stream/sdkconfig.defaults`.

The motivation text quoted generic ESP32 `iperf` figures (10-20 Mbps untuned, 30-50 Mbps
tuned). Those are not measurements of this firmware and are not used as evidence here.

This repository's own record points the other way. ADR-0010 decision 6 and
`docs/benchmark-results.md` ("Gate arithmetic", "Optimization deltas") show:

- peak observed link throughput **4.36 Mbps** on the softAP with one station;
- `app fps == device fps` within 0.3%, zero drops, so the PC pipeline is not binding;
- fps is the camera's frame-production rate (HD tops out near 11.2 fps; HD frame interval is
  linear in compressed frame size at ~2.2 ms/KiB).

ADR-0010 therefore dropped Wi-Fi/lwIP knobs with the instruction "re-open only if the link
becomes the constraint". This ADR re-opens them on the owner's instruction, **as an
experiment**, not because the link has been shown to be the constraint.

## Decision

Append the block below to `sdkconfig.defaults`. Each symbol's behaviour was checked in the
ESP-IDF v5.5.4 Kconfig sources and by generating a configuration (see Evidence).

| Symbol | Before (generated) | After (generated) | Real change? |
|---|---|---|---|
| `ESP_DEFAULT_CPU_FREQ_MHZ_240` | 160 MHz | **240 MHz** | yes |
| `ESPTOOLPY_FLASHMODE_QIO` | DIO | **QIO** | yes |
| `ESPTOOLPY_FLASHFREQ_80M` | 40 MHz | **80 MHz** | yes |
| `LWIP_TCP_SND_BUF_DEFAULT` | 5760 | **32768** | yes |
| `LWIP_TCP_WND_DEFAULT` | 5760 | **32768** | yes |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 32 | **64** | yes |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 32 | **64** | yes |
| `LWIP_IRAM_OPTIMIZATION` | n | **y** (needs ~13 KB IRAM; does not link alone) | yes |
| `FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` | n | **y** (frees IRAM so the row above links) | yes - **added after the build failed** |
| `ESP_WIFI_IRAM_OPT` | y | y | **no** - already the default here |
| `ESP_WIFI_RX_IRAM_OPT` | y | y | **no** - already the default here |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | n | n | **no** - stated explicitly |
| `SPIRAM_SPEED_40M` | y (40 MHz) | y (40 MHz) | **no** - pinned, see below |

Choices inside the owner's ranges, with the reason:

1. **TCP windows 32 KiB, not 65 KiB.** The owner's range was "at least 32 KB to 65 KB". The
   32 KiB end was chosen because these buffers draw on internal RAM, which sits next to PSRAM
   frame buffers and a record of brownouts under heavy streaming. Raise only on evidence.
2. **Wi-Fi dynamic buffers 64/64.** Espressif's own ESP32 iperf configuration
   (`examples/wifi/iperf/sdkconfig.defaults.esp32`, v5.5.4) uses 64/64. Range is up to 128.
3. **Flash 80 MHz does not move PSRAM.** `SPIRAM_SPEED_80M` becomes selectable once flash is
   80 MHz (ADR-0015). `CONFIG_SPIRAM_SPEED_40M=y` is already pinned explicitly, and the generated
   config confirms `SPIRAM_SPEED=40`. "Flash 80 / PSRAM 40" is combination 2 of the three
   supported combinations in `Kconfig.spiram`. PSRAM at 80 MHz remains the separate,
   unadopted lever of ADR-0015 decision 5.
4. **Frame buffers stay in PSRAM.** `CAM_DEFAULT_FB_LOCATION` is `CAMERA_FB_IN_PSRAM` and DRAM
   frame buffers are infeasible at SVGA and above (Phase 4). The "keep Wi-Fi DMA buffers in
   internal SRAM" item is satisfied by `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` staying off.

`FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` is Espressif's documented IRAM saver (non-ISR FreeRTOS
functions only). It is a second unmeasured change: task and queue calls now run from flash cache. It
exists only to make `LWIP_IRAM_OPTIMIZATION` fit, so the two are bisected together.

Not changed, deliberately: `LWIP_TCP_RECVMBOX_SIZE`, `ESP_WIFI_STATIC_RX_BUFFER_NUM`,
`ESP_WIFI_RX_BA_WIN`/`TX_BA_WIN`, `LWIP_EXTRA_IRAM_OPTIMIZATION`. They were not in the request
and the stream is transmit-dominated. They are the next candidates if this block shows a
link-side effect.

## Predictions (hypotheses, to be confirmed or refuted by measurement)

- At SVGA and below the camera is the limiter at well under the link rate, so fps is expected
  to be **unchanged**.
- At HD the encode interval, not the link, is expected to keep fps near 8-11, so fps is again
  expected to be **unchanged**; frame age p95 might improve slightly.
- Cost side: less free internal RAM, higher supply current at 240 MHz (brownout risk on the
  USB supply), less free IRAM.

If measurement confirms these, the block should be **reverted**, per the Phase 5 rule
(`docs/benchmark-plan.md`): anything that does not win on a real metric goes back.

## Measurement protocol (to run on hardware; nothing below has been run)

Baseline = `sdkconfig.defaults` at commit `5f1ef41`. One variable block at a time where
possible; the scene is fixed, because frame size - and so fps - tracks the scene (see
"Why HD measures 11-12 fps sometimes and 7-8 fps other times").

1. Build and flash the **baseline**: restore the old file
   (`git show 5f1ef41:firmware/esp32_cam_stream/sdkconfig.defaults > firmware/esp32_cam_stream/sdkconfig.defaults`),
   then `idf.py fullclean`, delete `sdkconfig`, `idf.py set-target esp32`, `idf.py build flash`.
2. Verify the configuration from the generated `sdkconfig`, not from the defaults file
   (ADR-0015): `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`, `CONFIG_ESPTOOLPY_FLASHFREQ="40m"`,
   `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5760`.
3. Run `python benchmarks/phase5_pipeline.py --framesize hd --quality 12 --seconds 120 --out <file>`
   and the same for `svga` / `--quality 36`, at least twice each. Record the device's
   `free_heap - free_spiram` (internal RAM) from `/api/v1/status` at start and end, plus
   `reset_reason`, `capture_failures`, `camera_recoveries`, and any change of `reset_reason`
   or drop of `uptime_s`.
4. Restore the new block, repeat steps 1-3 with expected values `240`, `"80m"`, `32768`.
5. Compare fps, frame bytes, Mbps, frame age p50/p95, internal-RAM headroom, resets.
   If a block must be bisected, comment lines out in this order (most network-related first):
   buffers, then `LWIP_IRAM_OPTIMIZATION` together with `FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`, then
   CPU, then flash QIO/80 MHz.
   The link itself is measured first and without the camera by `benchmarks/net_bench.py` against a
   `CONFIG_SORTING_CAM_NET_BENCH` build; see `docs/performance-workplan.md` phases 1-2.
6. Record both runs in `docs/benchmark-results.md` with firmware version, config, PC, date and
   duration, and update this ADR's status.

## Consequences

- Any machine that already has a git-ignored `sdkconfig` **keeps its old values** until it is
  regenerated. The firmware must be rebuilt from a clean `sdkconfig` to take this block.
- 80 MHz flash and 240 MHz CPU are new, unmeasured operating conditions on this hardware.
- The Phase 3-6 benchmark record was taken at 160 MHz / DIO 40 MHz / default lwIP and
  Wi-Fi buffers. It stays valid for that configuration and must not be quoted for this one.

## Evidence

Performed 2026-10-04, no hardware. ESP-IDF v5.5.4 (sources), Xtensa GCC 14.2.0
(`esp-14.2.0_20260121`), `esp32-camera` v2.1.7, `esp_jpeg` from `idf-extra-components`, target
`esp32`, each variant configured from a clean `sdkconfig` and built to a `.bin`. The build used a
scratch copy of the project with the two components vendored locally (no component manager), so it
proves that the sources and the configuration compile and link, not that the firmware runs.

| Variant | Defaults | Result | IRAM used / free (bytes) |
|---|---|---|---|
| A, production | new block | links, 1,019,000 B image | 121,147 / 9,925 |
| B, net bench, no camera | new block + bench | links | 120,879 / 10,193 |
| C, net bench, no camera | **baseline** defaults + bench | links | 118,163 / 12,909 |
| D, net bench, with camera | new block + bench | links | 120,879 / 10,193 |
| (first attempt) | new block without the FreeRTOS line | **link error**, iram0_0_seg overflowed by 440 B | n/a |
| (diagnostic) | new block minus `LWIP_IRAM_OPTIMIZATION` | links | 118,403 / 12,669 |

No warning was emitted for any project source (`main/*.c`) in any variant. The generated
`sdkconfig` of each variant was read back: A, B and D carry 240 MHz, `80m` flash in QIO, TCP
32768/32768, Wi-Fi buffers 64/64, and all three IRAM options; C carries the baseline 160 MHz, `40m`
DIO, 5760/5760, 32/32 and no lwIP IRAM option. `SPIRAM_SPEED` is 40 in all four.

Side effects of QIO/80 MHz seen in the generated configuration, none behavioural: the 1.8 V VDDSDIO
menu option is hidden at 80 MHz flash (`BOOTLOADER_VDDSDIO_BOOST_1_9V=y` in both); the custom
`SPIRAM_SPIWP_SD3_PIN` option is not offered because QIO owns the WP pin.

Flash mode: CONFIG_ESPTOOLPY_FLASHMODE="dio" with CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
is expected ESP-IDF behaviour, not a defect. The image header stays DIO so the ROM
can boot; on ESP32 the second-stage bootloader then enables QIO and the app flash
driver uses it. The 80m frequency is written into the header. Not yet verified on
hardware: the INFO field runtime.flash_io must read qio for new-block builds and
dio for baseline builds.

`ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT` default to `y` unless Bluetooth and PSRAM are both
enabled (`components/esp_wifi/Kconfig`); Bluetooth is not enabled here.

Espressif's ESP32 iperf reference sets QIO at **40 MHz**, not 80 MHz
(`examples/wifi/iperf/sdkconfig.defaults.esp32`), and uses 65534-byte TCP windows. 80 MHz flash is
therefore the owner's choice here, not Espressif's published throughput configuration.

### Phase 2, camera off (2026-10-09)

Run by `benchmarks/phase2_link.py`; every number read from
`benchmarks/results/phase2-link-baseline-20261009-121152.json` (NBB, baseline) and
`benchmarks/results/phase2-link-block-20261009-122431.json` (NB, block). Throughput
and per-second percentiles are medians over three repetitions per cell; `stalled s`
and `slow_sends` are totals over the three repetitions; `max_send ms` is a maximum
over repetitions; `min free internal` is a minimum over repetitions.

Firmware NBB — baseline:

| cell | valid reps | PC mean Mbps (median) | per-second p5 | per-second p50 | per-second p95 | stalled s | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int / chunk 8192 | 3 / 3 | 11.141 | 7.306 | 11.119 | 11.646 | 3 | 14 | 127.3 | 144287 |
| int / chunk 1460 | 3 / 3 | 7.502 | 6.624 | 7.717 | 8.129 | 4 | 13 | 563.5 | 144287 |
| int / chunk 32768 | 3 / 3 | 10.510 | 8.711 | 10.661 | 12.122 | 1 | 35 | 395.0 | 110735 |
| psram / chunk 8192 | 3 / 3 | 8.129 | 7.483 | 8.513 | 9.110 | 2 | 8 | 94.6 | 110735 |

Firmware NB — block:

| cell | valid reps | PC mean Mbps (median) | per-second p5 | per-second p50 | per-second p95 | stalled s | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int / chunk 8192 | 3 / 3 | 28.735 | 19.772 | 29.418 | 30.759 | 1 | 10 | 124.5 | 62119 |
| int / chunk 1460 | 3 / 3 | 20.155 | 19.376 | 20.061 | 20.929 | 0 | 0 | 22.0 | 47683 |
| int / chunk 32768 | 3 / 3 | 29.349 | 27.444 | 29.530 | 30.841 | 0 | 0 | 34.3 | 26259 |
| psram / chunk 8192 | 3 / 3 | 24.831 | 22.706 | 24.861 | 26.785 | 0 | 0 | 28.1 | 26259 |

INFO (before → after), baseline: cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; protocol_bitmap 7, HT40, channel 1,
max_tx_power_q 78; reset_reason 1 unchanged; uptime_s 113 → 540.
INFO (before → after), block: cpu_mhz 240; flash_mode_cfg "qio", flash_freq 80m,
runtime.flash_io qio; flash_id 0x5e4016; protocol_bitmap 7, HT40, channel 1,
max_tx_power_q 78; reset_reason 1 unchanged; uptime_s 101 → 528.

### Phase 2, camera on (2026-10-09)

Run by `benchmarks/phase2_cam.py` in run order NCB, NC, NCB; every number read from
`benchmarks/results/phase2-cam-baseline-20261009-193446.json` (run 1, NCB baseline),
`benchmarks/results/phase2-cam-block-20261009-195017.json` (run 2, NC block) and
`benchmarks/results/phase2-cam-baseline-20261009-202150.json` (run 3, NCB baseline
repeat); column definitions as in `docs/benchmark-results.md`, section
"Network bench, Phase 2 (camera on)".

Run 1 — baseline (NCB):

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 19.26 | 19.39 | 9.9 / 9.9 | 97.9 | 1.589 | 1 | 385 | 112003 |
| xga q24 @18 | 3 / 3 | 7.33 | 7.53 | 37.2 / 37.2 | 235.9 | 2.296 | 3 | 688 | 112003 |
| sxga q24 @18 | 3 / 3 | 9.00 | 8.95 | 37.2 / 37.2 | 210.1 | 2.728 | 0 | 747 | 111175 |
| uxga q36 @18 | 3 / 3 | 5.85 | 5.96 | 37.2 / 37.2 | 281.0 | 1.820 | 8 | 539 | 111175 |
| uxga q16 @18 | 3 / 3 | 7.19 | 6.98 | 37.2 / 37.2 | 261.9 | 2.131 | 0 | 600 | 111019 |

INFO (before → after), run 1 (NCB): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; protocol_bitmap 7, HT40, channel 1,
max_tx_power_q 78; reset_reason 1 unchanged; uptime_s 115 → 720.

Run 2 — block (NC):

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 22.48 | 22.57 | 9.9 / 10.0 | 69.5 | 1.817 | 3 | 1 | 54395 |
| xga q24 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 114.8 | 3.453 | 0 | 29 | 40783 |
| sxga q24 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 115.4 | 3.457 | 0 | 29 | 40783 |
| uxga q36 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 130.0 | 3.455 | 0 | 110 | 40783 |
| uxga q16 @18 | 3 / 3 | 11.22 | 11.29 | 37.2 / 37.2 | 126.4 | 3.442 | 1 | 260 | 36771 |

INFO (before → after), run 2 (NC): cpu_mhz 240; flash_mode_cfg "qio", flash_freq 80m,
runtime.flash_io qio; flash_id 0x5e4016; protocol_bitmap 7, HT40, channel 1,
max_tx_power_q 78; reset_reason 1 unchanged; uptime_s 71 → 670.

Run 3 — baseline repeat (NCB):

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 22.48 | 22.52 | 9.9 / 10.0 | 66.7 | 1.824 | 0 | 35 | 112907 |
| xga q24 @18 | 3 / 3 | 10.52 | 10.58 | 37.2 / 37.2 | 145.7 | 3.227 | 0 | 906 | 112907 |
| sxga q24 @18 | 3 / 3 | 10.81 | 10.89 | 37.2 / 37.2 | 132.5 | 3.321 | 1 | 888 | 111403 |
| uxga q36 @18 | 3 / 3 | 10.15 | 10.10 | 37.2 / 37.2 | 155.8 | 3.081 | 2 | 894 | 111403 |
| uxga q16 @18 | 3 / 3 | 10.41 | 10.41 | 37.2 / 37.2 | 151.7 | 3.177 | 0 | 911 | 111403 |

INFO (before → after), run 3 (NCB): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; protocol_bitmap 7, HT40, channel 1,
max_tx_power_q 78; reset_reason 1 unchanged; uptime_s 131 → 733.

Measured on black frames (ADR-0019); to be repeated before any conclusion about fps.

Not done: for the 2026-10-04 record nothing was flashed, so no frame rate, current draw or
brownout behaviour of any variant is known. Camera-off throughput and camera-on stream
rates were measured on 2026-10-09 in the bench build (tables above); the size of the frames the
sensor actually produced is under verification; nothing has been measured in the production
firmware.

## Related

- ADR-0010 decision 6 (Wi-Fi/lwIP knobs not pursued) - re-opened by this ADR as a trial
- ADR-0015 (PSRAM clock, silent symbol discard, regenerate-to-verify rule)
- `docs/benchmark-results.md` "Optimization deltas"; `docs/benchmark-plan.md` Phase 5 protocol
- `docs/performance-workplan.md` (how this trial fits the fps/quality goal) and
  `benchmarks/net_bench.py` + `firmware/esp32_cam_stream/main/net_bench.c` (the link measurement)
