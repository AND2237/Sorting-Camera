# ADR-0009: Frame budget, quality floor, and camera recovery

_Status: accepted 2026-09-26_

## Context

A viewer that stopped, changed resolution, and reconnected could leave the camera permanently dead: every later connection got a TCP handshake but no data, and every config change was refused with 409. Root cause chain, all confirmed from the device log and our own measurements:

1. `esp32-camera` in JPEG mode sizes every frame buffer from `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_AUTO` = `width*height/5` (the `camera_config_t.fb_size` field is **ignored** in this path). Measured fill at quality 4 was 71–99% of that buffer, and quality 0–3 exceeded it.
2. On overflow the driver logs `cam_hal: FB-OVF` and `esp_camera_fb_get()` then only ever returns NULL after its ~4.5 s internal timeout. The sensor was left in that state until power-cycle.
3. The stream handler tolerated 50 consecutive capture failures before closing the client — about 225 s inside the stream httpd task, which is single-threaded, so no new connection could be served and `stream_clients` stayed non-zero, which is what produced the endless 409s.
4. The stall watchdog only ran while a client was attached, so a camera left broken while nobody was watching stayed broken; the next connect could never succeed.
5. Every config change — including a quality-slider drag, which fires per pixel — did a full `esp_camera_deinit()`/`esp_camera_init()`.

## Decision

1. **Explicit frame budget.** Build with `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM` and a 262,144-byte budget (256 KiB) instead of the driver's AUTO sizing. (`sdkconfig.defaults` sets 262144; this line once read "256,144", a typo — corrected 2026-09-30 so nobody "corrects" the firmware to match it.) A 512 KiB budget was tried first and rejected: it pushed the driver's DMA node count from 23 to 128, cost 1.28 MB of internal DRAM (free heap 4.0 MB → 2.7 MB) and coincided with brownout resets under maximum-size frames.
2. **Measured quality floors, no extrapolation.** A linear size-vs-quality model was implemented, measured to be wrong (it predicted 72 KB for SVGA q0 where the real frame was 116 KB), and removed. Each resolution instead carries a floor and a measured maximum from a 10 s stream (44–144 frames/cell). The config endpoint rejects anything below the floor with **400** and names it; the app clamps its slider to `quality_floor` from the status API. Floors whose margin fell below 2x under observed scene variation were raised (UXGA q12 → q16).
3. **Recover, do not brick.** `camera_recover()` (deinit → PWDN power-cycle → SCCB recovery → init) is called by the stream handler on the first capture failure, by the snapshot handler, and by the watchdogs. `esp_restart()` remains only as the last resort when recovery itself fails. `camera_recoveries` is exported in the status API.
4. **Bounded failure path.** The handler gives up after one failed capture plus two recovery attempts instead of 50 silent retries, so the httpd task is never pinned for minutes.
5. **Quality without reinit.** A quality-only change is applied through `sensor->set_quality()`; deinit/init is reserved for framesize, fb_count, fb location, grab mode and xclk.
6. **Persist the operating point** in NVS so a watchdog reboot does not silently change the camera configuration.

## Consequences

- The FB-OVF brick class is closed for every configuration the API accepts, and the residual case (a scene far more complex than anything measured) now degrades into a ~1 s pause instead of a dead device.
- Quality is bounded per resolution (QQVGA/QVGA q0, VGA/SVGA q2, XGA/HD q6, SXGA q8, UXGA q16). High quality also costs frame rate (1.5–10 fps at the floors vs 19–45 fps at q12–q36) because the sensor encodes larger JPEGs; the throughput ladder from ADR-0008 is unaffected.
- 768 KB of the 4 MB mapped PSRAM is permanently reserved for frame buffers.
- Frame size remains scene-dependent: verification runs saw up to ~2x the 10 s maximum for the same cell. The guard reduces this risk, it does not eliminate it; the near-budget warning (`frame >= 90% of budget`) and the recovery path are the safety nets.

## Evidence

- `benchmarks/results/qlow-20260926.json`, `benchmarks/results/qfloor-20260926.json` — floor calibration.
- `docs/benchmark-results.md` — the incident, the measured floors, and the verification runs.
- Serial logs of 2026-09-26: pre-fix `FB-OVF` storm with `Failed to get frame: timeout` every 4.5 s and `accept (23)` storms; post-fix runs with `recoveries=0, capture_failures=0` across 10/10 connect→disconnect→change→connect cycles and 20/20 rapid connect/abort cycles.
