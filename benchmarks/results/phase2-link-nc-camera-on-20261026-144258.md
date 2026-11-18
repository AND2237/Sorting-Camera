# Phase 2 link test - nc-camera-on (expected build: new)

_Run 2026-10-26T14:47:45; 30 s per run, warm-up 3 s, 2 repetitions per cell, retries 2. Measurements only; no conclusion drawn._

## Device (INFO before the matrix)

```json
{
  "fw": "netbench",
  "uptime_s": 62,
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
    "free_internal": 138155,
    "min_free_internal": 131803,
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

| Cell | valid reps | PC mean Mbps (median) | all reps | per-second p5 / p50 / p95 | stalled s | device Mbps | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int @8192 | 2/2 | 17.24 | [17.093, 17.382] | 15.53 / 17.36 / 18.50 | 0 | 17.24 | 1 | 59.9 | 22227 |
| int @1460 | 2/2 | 7.85 | [7.797, 7.895] | 4.37 / 7.06 / 11.92 | 0 | 7.85 | 171 | 226.3 | 10631 |
| int @32768 | 2/2 | 17.69 | [17.461, 17.916] | 16.44 / 17.68 / 19.36 | 0 | 17.68 | 2 | 69.5 | 435 |
| psram @8192 | 2/2 | 9.12 | [6.769, 11.466] | 4.62 / 10.12 / 12.10 | 10 | 9.12 | 127 | 1350.5 | 435 |

## Invalid or failed attempts

- none

## Device after the matrix

- reset_reason power-on, uptime 349 s
- min free internal RAM ever: 435
