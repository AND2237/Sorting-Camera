# Exposure sweep - room

_Run 2026-10-12T11:58:44; 10 s per run, warm-up 3 s, 2 repetitions per cell. Real frames over the bench port. The scene and lighting named by the label were kept fixed. Measurements only._

## Device (INFO before the sweep)

```json
{
  "fw": "netbench",
  "uptime_s": 1011,
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
    "free_internal": 137631,
    "min_free_internal": 113775,
    "free_spiram": 3403764
  },
  "last": {
    "runs": 6,
    "finished": 1,
    "ok": 1,
    "mode": "cam",
    "secs_req": 10,
    "chunk": 8192,
    "secs": 10.078,
    "bytes": 6496360,
    "mbps": 5.157,
    "frames": 73,
    "fps": 7.243,
    "sends": 146,
    "slow_sends": 73,
    "max_send_ms": 186.5,
    "cam_fail": 0,
    "free_internal": 134399,
    "min_free_internal": 113775
  }
}
```

## Sensor controls before the sweep (restored afterwards)

```json
{
  "brightness": 0,
  "contrast": 0,
  "saturation": 0,
  "ae_level": 0,
  "special_effect": 0,
  "wb_mode": 0,
  "aec_value": 300,
  "agc_gain": 0,
  "gainceiling": 0,
  "agc": 1,
  "aec": 1,
  "aec2": 1,
  "awb": 1,
  "awb_gain": 1,
  "hmirror": 0,
  "vflip": 0,
  "bpc": 1,
  "wpc": 1,
  "raw_gma": 1,
  "lenc": 1,
  "dcw": 1,
  "colorbar": 0
}
```

## Results (median over valid repetitions)

| Profile | Set | valid reps | fps | all reps | longest gap ms | gap p95 ms | frame KiB | link Mbps | brightness (0-255) |
|---|---|---|---|---|---|---|---|---|---|
| xga q24 @18 | auto-default | 2/2 | 11.29 | [11.29, 11.29] | 151 | 113 | 27.2 | 2.54 | 106 |
| xga q24 @18 | auto-gain32x | 2/2 | 11.29 | [11.29, 11.29] | 152 | 114 | 27.2 | 2.54 | 107 |
| xga q24 @18 | auto-gain128x | 2/2 | 11.21 | [11.29, 11.14] | 170 | 114 | 27.2 | 2.54 | 107 |
| xga q24 @18 | manual-e300-g0 | 2/2 | 10.93 | [11.14, 10.71] | 209 | 137 | 26.1 | 2.39 | 93 |
| xga q24 @18 | manual-e600-g10 | 2/2 | 11.29 | [11.29, 11.29] | 130 | 107 | 19.8 | 1.86 | 242 |
| xga q24 @18 | manual-e1000-g20 | 2/2 | 11.21 | [11.29, 11.14] | 121 | 106 | 15.8 | 1.48 | 254 |

## Settings in each set

- **auto-default**: {'aec': 1, 'aec2': 1, 'agc': 1, 'gainceiling': 0, 'ae_level': 0}
- **auto-gain32x**: {'aec': 1, 'aec2': 1, 'agc': 1, 'gainceiling': 4}
- **auto-gain128x**: {'aec': 1, 'aec2': 1, 'agc': 1, 'gainceiling': 6}
- **manual-e300-g0**: {'aec': 0, 'aec2': 0, 'aec_value': 300, 'agc': 0, 'agc_gain': 0}
- **manual-e600-g10**: {'aec': 0, 'aec2': 0, 'aec_value': 600, 'agc': 0, 'agc_gain': 10}
- **manual-e1000-g20**: {'aec': 0, 'aec2': 0, 'aec_value': 1000, 'agc': 0, 'agc_gain': 20}

## Notes, refused sets and failed attempts

- none

## Device after the sweep

- reset_reason power-on, uptime 1194 s
