# Phase 2 camera-on link test - baseline (expected build: baseline)

_Run 2026-10-09T19:44:50; 30 s per run, warm-up 3 s, 3 repetitions per profile, retries 2. Real camera frames over the bench port (no HTTP server in the path). Same scene required for every firmware compared. Measurements only; no conclusion drawn._

## Device (INFO before the matrix)

```json
{
  "fw": "netbench",
  "uptime_s": 115,
  "reset_reason": 1,
  "camera_enabled": 1,
  "build": {
    "cpu_mhz": 160,
    "flash_freq": "40m",
    "flash_mode_cfg": "not-qio",
    "psram_mhz": 40,
    "tcp_snd_buf": 5760,
    "tcp_wnd": 5760,
    "wifi_rx_buf": 32,
    "wifi_tx_buf": 32,
    "wifi_iram": 1,
    "wifi_rx_iram": 1,
    "lwip_iram": 0,
    "freertos_in_flash": 0
  },
  "runtime": {
    "flash_io": "dio",
    "flash_id": "0x5e4016",
    "flash_size": 4194304,
    "protocol_bitmap": 7,
    "bandwidth": "HT40",
    "channel": 1,
    "max_tx_power_q": 78,
    "free_internal": 136407,
    "min_free_internal": 114983,
    "free_spiram": 3403748
  },
  "last": {
    "runs": 1,
    "finished": 1,
    "ok": 1,
    "mode": "cam",
    "secs_req": 10,
    "chunk": 8192,
    "secs": 10.023,
    "bytes": 1954188,
    "mbps": 1.56,
    "frames": 183,
    "fps": 18.258,
    "sends": 366,
    "slow_sends": 43,
    "max_send_ms": 821.1,
    "cam_fail": 0,
    "free_internal": 137063,
    "min_free_internal": 114983
  }
}
```

## Results (median over valid repetitions)

| Profile | valid reps | fps (PC) | all reps | fps (device) | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3/3 | 19.26 | [19.26, 19.0, 21.26] | 19.39 | 9.9 / 9.9 | 97.9 | 1.59 | 1 | 385 | 112003 |
| xga q24 @18 | 3/3 | 7.33 | [7.33, 7.15, 8.44] | 7.53 | 37.2 / 37.2 | 235.9 | 2.30 | 3 | 688 | 112003 |
| sxga q24 @18 | 3/3 | 9.00 | [9.0, 9.0, 6.85] | 8.95 | 37.2 / 37.2 | 210.1 | 2.73 | 0 | 747 | 111175 |
| uxga q36 @18 | 3/3 | 5.85 | [4.89, 6.93, 5.85] | 5.96 | 37.2 / 37.2 | 281.0 | 1.82 | 8 | 539 | 111175 |
| uxga q16 @18 | 3/3 | 7.19 | [7.19, 7.37, 5.81] | 6.98 | 37.2 / 37.2 | 261.9 | 2.13 | 0 | 600 | 111019 |

## Invalid or failed attempts

- none

## Device after the matrix

- reset_reason power-on, uptime 720 s
- min free internal RAM ever: 111019
