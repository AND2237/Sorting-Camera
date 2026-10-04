# ADR-0018: Wi-Fi / TCP throughput tuning block in `sdkconfig.defaults` (trial, unmeasured)

_Status: adopted as a **trial** 2026-10-04; hardware measurement pending. No result in this
document is a measurement of the camera._

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
| `LWIP_IRAM_OPTIMIZATION` | n | **y** (~10 KB IRAM) | yes |
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
   `reset_reason`, `capture_failures`, `camera_recoveries`, and any `E BOD` serial lines.
4. Restore the new block, repeat steps 1-3 with expected values `240`, `"80m"`, `32768`.
5. Compare fps, frame bytes, Mbps, frame age p50/p95, internal-RAM headroom, resets.
   If a block must be bisected, comment lines out in this order (most network-related first):
   buffers + IRAM, then CPU, then flash QIO/80 MHz.
6. Record both runs in `docs/benchmark-results.md` with firmware version, config, PC, date and
   duration, and update this ADR's status.

## Consequences

- Any machine that already has a git-ignored `sdkconfig` **keeps its old values** until it is
  regenerated. The firmware must be rebuilt from a clean `sdkconfig` to take this block.
- 80 MHz flash and 240 MHz CPU are new, unmeasured operating conditions on this hardware.
- The Phase 3-6 benchmark record was taken at 160 MHz / DIO 40 MHz / default lwIP and
  Wi-Fi buffers. It stays valid for that configuration and must not be quoted for this one.

## Evidence

Performed 2026-10-04 off-target (no hardware, no build): ESP-IDF v5.5.4 sources,
`esp-idf-kconfig` 3.13.0 `kconfgen`, every `components/*/Kconfig[.projbuild]` plus
`esp32-camera` v2.1.7 and this project's `Kconfig.projbuild`, target `esp32`. This is a
superset of the components a real build links, so it proves symbol validity and
non-discard, not the exact final `sdkconfig`.

- Generating from the baseline defaults and from the new defaults and diffing the results:
  all eight "real change" symbols took effect; the four "no change" symbols were identical;
  **no requested value was silently discarded** (the ADR-0015 hazard).
- Side effects of QIO/80 MHz seen in the diff, none behavioural: the 1.8 V VDDSDIO menu option
  is hidden at 80 MHz flash (the generated choice stays `BOOTLOADER_VDDSDIO_BOOST_1_9V=y` in
  both); `BOOTLOADER_SPI_WP_PIN=7` appears and `SPIRAM_SPIWP_SD3_PIN` disappears (QIO owns the
  WP pin; the custom-pin option stays off).
- `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT` default to `y` unless Bluetooth and PSRAM are both
  enabled (`components/esp_wifi/Kconfig`); Bluetooth is not enabled here.
- Espressif's ESP32 iperf reference sets QIO at **40 MHz**, not 80 MHz
  (`examples/wifi/iperf/sdkconfig.defaults.esp32`). 80 MHz flash is therefore the owner's
  choice here, not Espressif's published throughput configuration.

## Related

- ADR-0010 decision 6 (Wi-Fi/lwIP knobs not pursued) - re-opened by this ADR as a trial
- ADR-0015 (PSRAM clock, silent symbol discard, regenerate-to-verify rule)
- `docs/benchmark-results.md` "Optimization deltas"; `docs/benchmark-plan.md` Phase 5 protocol
