# Phase 2 link test - baseline (expected build: baseline)

_Run 2026-10-09T12:19:00; 30 s per run, warm-up 3 s, 3 repetitions per cell, retries 2. Measurements only; no conclusion drawn._

## Device (INFO before the matrix)

```json
{
  "fw": "netbench",
  "uptime_s": 113,
  "reset_reason": 1,
  "camera_enabled": 0,
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
    "free_internal": 176567,
    "min_free_internal": 170443,
    "free_spiram": 4191636
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

| Cell | valid reps | PC mean Mbps (median) | all reps | per-second p5 / p50 / p95 | stalled s | device Mbps | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int @8192 | 3/3 | 11.14 | [11.193, 11.141, 10.654] | 7.31 / 11.12 / 11.65 | 3 | 11.14 | 14 | 127.3 | 144287 |
| int @1460 | 3/3 | 7.50 | [7.779, 6.843, 7.502] | 6.62 / 7.72 / 8.13 | 4 | 7.50 | 13 | 563.5 | 144287 |
| int @32768 | 3/3 | 10.51 | [10.008, 11.27, 10.51] | 8.71 / 10.66 / 12.12 | 1 | 10.51 | 35 | 395.0 | 110735 |
| psram @8192 | 3/3 | 8.13 | [7.916, 8.129, 8.527] | 7.48 / 8.51 / 9.11 | 2 | 8.13 | 8 | 94.6 | 110735 |

## Invalid or failed attempts

- none

## Device after the matrix

- reset_reason power-on, uptime 540 s
- min free internal RAM ever: 110735
