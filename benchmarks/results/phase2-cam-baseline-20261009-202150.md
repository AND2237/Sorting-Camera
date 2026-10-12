# Phase 2 camera-on link test - baseline (expected build: baseline)

_Run 2026-10-09T20:31:52; 30 s per run, warm-up 3 s, 3 repetitions per profile, retries 2. Real camera frames over the bench port (no HTTP server in the path). Same scene required for every firmware compared. Measurements only; no conclusion drawn._

## Device (INFO before the matrix)

```json
{
  "fw": "netbench",
  "uptime_s": 131,
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
    "free_internal": 138159,
    "min_free_internal": 131811,
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

| Profile | valid reps | fps (PC) | all reps | fps (device) | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3/3 | 22.48 | [22.52, 21.96, 22.48] | 22.52 | 9.9 / 10.0 | 66.7 | 1.82 | 0 | 35 | 112907 |
| xga q24 @18 | 3/3 | 10.52 | [10.93, 10.33, 10.52] | 10.58 | 37.2 / 37.2 | 145.7 | 3.23 | 0 | 906 | 112907 |
| sxga q24 @18 | 3/3 | 10.81 | [10.81, 9.44, 11.19] | 10.89 | 37.2 / 37.2 | 132.5 | 3.32 | 1 | 888 | 111403 |
| uxga q36 @18 | 3/3 | 10.15 | [9.89, 10.15, 10.26] | 10.10 | 37.2 / 37.2 | 155.8 | 3.08 | 2 | 894 | 111403 |
| uxga q16 @18 | 3/3 | 10.41 | [9.93, 10.48, 10.41] | 10.41 | 37.2 / 37.2 | 151.7 | 3.18 | 0 | 911 | 111403 |

## Invalid or failed attempts

- none

## Device after the matrix

- reset_reason power-on, uptime 733 s
- min free internal RAM ever: 111403
