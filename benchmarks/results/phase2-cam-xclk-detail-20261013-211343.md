# Phase 2 camera-on link test - xclk-detail (expected build: new)

_Run 2026-10-13T21:23:30; 20 s per run, warm-up 3 s, 2 repetitions per profile, retries 0. Real camera frames over the bench port (no HTTP server in the path). Same scene required for every firmware compared. Measurements only; no conclusion drawn._

## Device (INFO before the matrix)

```json
{
  "fw": "netbench",
  "uptime_s": 758,
  "reset_reason": 1,
  "camera_enabled": 1,
  "build": {
    "cpu_mhz": 240,
    "flash_freq": "80m",
    "flash_mode_cfg": "qio",
    "psram_mhz": 40,
    "tcp_snd_buf": 32768,
    "tcp_wnd": 32768,
    "wifi_rx_buf": 64,
    "wifi_tx_buf": 64,
    "wifi_iram": 1,
    "wifi_rx_iram": 1,
    "lwip_iram": 1,
    "freertos_in_flash": 1
  },
  "runtime": {
    "flash_io": "qio",
    "flash_id": "0x5e4016",
    "flash_size": 4194304,
    "protocol_bitmap": 7,
    "bandwidth": "HT40",
    "channel": 1,
    "max_tx_power_q": 78,
    "free_internal": 138183,
    "min_free_internal": 100787,
    "free_spiram": 3403748
  },
  "last": {
    "runs": 0,
    "finished": 0,
    "ok": 0,
    "mode": "",
    "secs_req": 0,
    "chunk": 0,
    "secs": 0.0,
    "bytes": 0,
    "mbps": 0.0,
    "frames": 0,
    "fps": 0.0,
    "sends": 0,
    "slow_sends": 0,
    "max_send_ms": 0.0,
    "cam_fail": 0,
    "free_internal": 0,
    "min_free_internal": 0
  }
}
```

## Results (median over valid repetitions)

| Profile | valid reps | fps (PC) | all reps | fps (device) | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal | actual WxH | end-marker ok % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uxga q16 @18 | 2/2 | 10.44 | [9.65, 11.24] | 10.66 | 64.7 / 76.8 | 162.6 | 5.78 | 2 | 119 | 46931 | 1600x1200 | 100.0 |
| uxga q16 @20 | 1/2 | 12.53 | [12.53] | 12.55 | 59.0 / 60.8 | 106.8 | 6.05 | 0 | 8 | 46871 | 1600x1200 | 100.0 |
| uxga q16 @22 | 2/2 | 5.97 | [5.88, 6.06] | 5.95 | 228.1 / 231.9 | 278.2 | 11.13 | 0 | 240 | 43663 | 1600x1200 | 100.0 |
| uxga q16 @24 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |
| xga q12 @18 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |
| xga q12 @20 | 2/2 | 12.47 | [12.47, 12.47] | 12.64 | 29.8 / 29.9 | 88.5 | 3.08 | 0 | 1 | 59959 | 1024x768 | 100.0 |
| xga q12 @22 | 2/2 | 13.79 | [13.76, 13.82] | 13.87 | 59.1 / 60.4 | 84.6 | 6.72 | 0 | 2 | 45775 | 1024x768 | 100.0 |
| xga q12 @24 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |

## Invalid or failed attempts

- uxga q16 @20 rep 2 try 1: no data received (recv timeout (device stopped sending)); is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y, and is the mode valid for this build (mode cam needs the camera enabled)?
- uxga q16 @24 rep 1 try 1: no data received (recv timeout (device stopped sending)); is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y, and is the mode valid for this build (mode cam needs the camera enabled)?
- uxga q16 @24 rep 2 try 1: cannot read config back
- xga q12 @18 rep 1 try 1: config POST failed (HTTP 0): [WinError 10060] A connection attempt failed because the connected party did not properly respond after a period of time
- xga q12 @18 rep 2 try 1: config POST failed (HTTP 0): [WinError 10060] A connection attempt failed because the connected party did not properly respond after a period of time
- xga q12 @24 rep 1 try 1: no data received (recv timeout (device stopped sending)); is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y, and is the mode valid for this build (mode cam needs the camera enabled)?
- xga q12 @24 rep 2 try 1: no data received (recv timeout (device stopped sending)); is the board flashed with CONFIG_SORTING_CAM_NET_BENCH=y, and is the mode valid for this build (mode cam needs the camera enabled)?

## Device after the matrix

- reset_reason panic, uptime 378 s
- min free internal RAM ever: 45775
