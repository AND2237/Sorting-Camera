# Benchmark results

Harness-run numbers are filled only by actual runs of `benchmarks/` harnesses.
Fabricated or estimated numbers are forbidden.

## Manual tuning session (pre-Phase-3, interactive)

Not a harness run — FPS read from the app UI / frame counters during interactive
tuning; superseded later by Phase 3/4 harness data.

- **Date:** 2026-09-24
- **Firmware:** local softAP build (uncommitted working tree, `fw_version` field "1"), esp32-camera via ESP-IDF v5.5.4
- **App:** local Qt 6.11.2 MinGW build (uncommitted working tree), HTTP MJPEG (chunked decoder)
- **Config baseline:** OV2640, q12, fb_count 3, `CAMERA_GRAB_LATEST`, ESP32 softAP `192.168.4.1` ch1
- **PC:** Intel i3-1215U, Windows 10.0.26200, Intel AX201 (driver 23.60.0.10), joined to camera AP (RSSI −37…−45 dBm)

| Resolution | XCLK | FPS (observed) | Noise | Notes |
|------------|------|----------------|-------|-------|
| UXGA 1600×1200 | 20 MHz | 2.5 (76 frames / 30 s counted) | present | user-rejected on quality |
| UXGA 1600×1200 | 14 MHz | 0.4–1 | gone | |
| SVGA 800×600 | 14 MHz | 6–10 | gone | 589 captured / 587 delivered / 0 fail |
| SVGA 800×600 | 18 MHz | 16–22 | clean | only config meeting ≥15 floor |
| HD 1280×720 | 20 MHz | 2–3 | unacceptable | noise inflates JPEG → Wi-Fi stall loop |
| HD 1280×720 | 19 MHz | — | unacceptable | noise returned |
| HD 1280×720 | 18 MHz | 9–11 | almost gone | **locked boot default** |

**Findings:** (1) this board's max clean XCLK is 18 MHz — 19/20 MHz produce
visible sensor noise; (2) HD ≥15 fps clean would require ~27 MHz — out of the
clean range, so quality-first priority selects HD@18 (~10 fps) over SVGA@18;
(3) changing `framesize` while a stream client is active wedged the HTTP
control server (no panic, no reboot) — config changes must be issued with
`stream_clients=0` until firmware rejects them (hardening TODO).

## Run log

| Run | Date | FW ver | App ver | Transport | Resolution | Quality | fb/grab/xclk | Router/PC | Duration | Result summary |
|-----|------|--------|---------|-----------|------------|---------|--------------|-----------|----------|----------------|
| P3-A-http | 2026-09-25 | `6444e6d-dirty` | harness (no app) | HTTP MJPEG :81 | 640×480 | 12 | 3 / latest / 18 MHz | softAP ch1, RSSI −33, i3-1215U/AX201 | 10 s + 120 s | 2462 frames, 22.4 fps, 0 drops |
| P3-A-tcp | 2026-09-25 | `6444e6d-dirty` | harness | TCP framed :82 | 640×480 | 12 | 3 / latest / 18 MHz | same | 10 s + 120 s | 2476 frames, 22.5 fps, 0 drops |
| P3-A-udp | 2026-09-25 | `6444e6d-dirty` | harness | UDP packetized :8500 | 640×480 | 12 | 3 / latest / 18 MHz | same | 10 s + 120 s | 1888 frames, 17.2 fps, 1 incomplete frame |
| P3-B-http | 2026-09-25 | `6444e6d-dirty` | harness | HTTP MJPEG :81 | 1280×720 | 12 | 3 / latest / 18 MHz | same | 10 s + 120 s | 1217 frames, 11.1 fps, 0 drops |
| P3-B-tcp | 2026-09-25 | `6444e6d-dirty` | harness | TCP framed :82 | 1280×720 | 12 | 3 / latest / 18 MHz | same | 10 s + 120 s | 979 frames, 8.9 fps, 0 drops |
| P3-B-udp | 2026-09-25 | `6444e6d-dirty` | harness | UDP packetized :8500 | 1280×720 | 12 | 3 / latest / 18 MHz | same | 10 s + 120 s | 1209 frames, 11.0 fps, 34 incomplete frames |
| P4-matrix | 2026-09-26 | `741eeb8-dirty` | harness (`phase4_matrix.py`) | HTTP MJPEG :81 | 7 res × 4 qual + fb/grab cross + 130 s confirms | 4–36 | 1–3 / latest+cont / 18 MHz | softAP ch1, RSSI −33…−36, i3-1215U/AX201 | 28 × 55 s + 12 × 55 s + 11 × 130 s | envelope+cross+confirm complete; 6-point ladder; 1 dead cell (qvga/q4 fb-overflow) |

Raw JSON per run: `benchmarks/results/phase3-20260925-*.json`
(multi-client probe: `benchmarks/results/phase3-multi-client.json`).

## Phase 3 — transport shootout (2026-09-25)

- **Method:** `benchmarks/transport_bench.py` (stdlib receive-only), orchestrated
  by `benchmarks/run_phase3.py`. Config A = 640×480/q12, Config B = 1280×720/q12
  (boot default HD is config B); XCLK 18 MHz, fb_count 3, GRAB_LATEST for both.
  10 s warm-up, 120 s measured, one client per run, runs sequential with 3 s cooldown.
  PC CPU = harness process only (no JPEG decode in harness; decode is app-side, later).
- **Latency:** device capture-complete timestamp (HTTP `X-Capture-Us`, TCP header
  `timestamp_us`) vs PC receive time; clock offset from UDP RTT-sync channel
  (5 samples/run, midpoint method, median RTT 5–17 ms). **NTP-less estimate** —
  accurate to ~half the RTT jitter. **UDP has no per-frame device timestamp in
  protocol v1 → latency not estimated for UDP** (RTT only).
- **PC:** Intel i3-1215U, Windows 10.0.26200, AX201, direct softAP link (ADR-0006), RSSI −33…−36 dBm.
- **Firmware:** `frame_transport.c` (TCP 36-byte SCAM header + CRC32, UDP 1400 B
  datagrams, START keep-alive 1 s, peer timeout 3 s), `X-Capture-Us` added to MJPEG.

### Config A — 640×480, q12, xclk 18 MHz

| Transport | frames | fps mean / p50 / p95 | interval p95 (ms) | frame B p50 | throughput | drops (gap/incomp/crc) | latency p50 / p95 (ms) | PC CPU % | system CPU % |
|---|---|---|---|---|---|---|---|---|---|
| HTTP MJPEG | 2462 | 22.38 / 23 / 23 | 67.7 | 15313 | 2.96 Mbps | 0 / 0 / 0 | 36.5 / 56.2 | 2.04 | 10.67 |
| TCP framed | 2476 | 22.50 / 23 / 23 | 70.7 | 12903 | 2.66 Mbps | 0 / 0 / 0 | **31.1 / 53.2** | 1.59 | 10.05 |
| UDP packetized | 1888 | 17.16 / 17 / 18 | 74.3 | 13445 | 2.01 Mbps | 0 / **1** / 0 | n/a | **0.92** | 10.31 |

### Config B — 1280×720, q12, xclk 18 MHz (locked default)

| Transport | frames | fps mean / p50 / p95 | interval p95 (ms) | frame B p50 | throughput | drops (gap/incomp/crc) | latency p50 / p95 (ms) | PC CPU % | system CPU % |
|---|---|---|---|---|---|---|---|---|---|
| HTTP MJPEG | 1217 | **11.06** / 11 / 12 | 121.5 | 44397 | 4.32 Mbps | 0 / 0 / 0 | **92.7 / 123.0** | 2.16 | 12.52 |
| TCP framed | 979 | 8.90 / 9 / 11 | 162.0 | 49932 | 3.95 Mbps | 0 / 0 / 0 | 118.6 / 178.1 | 1.58 | 11.39 |
| UDP packetized | 1209 | 11.00 / 11 / 12 | 120.8 | 49095 | 4.74 Mbps | 0 / **34** / 0 | n/a | 1.89 | 10.10 |

**Notes on interpretation:**

1. At VGA, HTTP and TCP are camera-bound and effectively tied (22.4 vs 22.5 fps);
   frame-size p50 differs (15.3 vs 12.9 KB) because runs are separate samples of
   a varying JPEG source — fps differences of this order between runs are content,
   not protocol.
2. At HD, TCP's lower fps (8.9 vs 11.1) tracks its 13% larger median frame
   (49.9 vs 44.4 KB) at similar throughput — single run per cell, confounded by
   frame size; treat HTTP/TCP HD fps as "same envelope, ~9–11 fps" rather than a
   protocol ranking. Latency at HD favored HTTP in these runs (92.7 vs 118.6 ms p50).
3. UDP is measurably slower on fps at VGA (−24%) from per-fragment send overhead
   (10–12 datagrams/frame) but lowest PC CPU; loss was near-zero at VGA
   (1 incomplete frame / 120 s) and 2.7% at HD (34 frames), 0 device-side send drops.
4. All six runs: **0 corrupt frames** (CRC verified on TCP/UDP; HTTP has no payload
   CRC — unverifiable by design), 0 reboots, counters continuous.

### Multi-client probe (2 clients, ~15 s, not a ship gate)

| Transport | Client A frames | Client B frames (joins +2 s) | Behavior |
|---|---|---|---|
| HTTP | 144 | 5 | second request effectively starved — ESP-IDF httpd serves one request at a time |
| TCP | 164 | 5 | single client by design: second waits in backlog until first disconnects |
| UDP | 100 | 77 | single peer v1: last START wins, first client stops receiving |

### Stability pass (per `benchmark-plan.md` Phase 3)

- **Client kill-while-connected (TCP + HTTP, SO_LINGER 0 → RST mid-frame):**
  device counters returned to `tcp_clients=0`/`stream_clients=0`, uptime continuous,
  next stream clean with all CRCs valid. Bounded recovery — **pass**.
- **Abrupt kill → immediate reconnect:** one transient `ConnectionReset` observed
  within seconds of the kill; retry ~3 s later succeeded (54/59 frames, all valid).
  Bounded, no wedge — noted as a reconnect-retry requirement for client code.
- **AP disruption (PC Wi-Fi off ~80 s → rejoin) + camera reboot (RST), with a TCP
  listener attached throughout:** listener recovered after rejoin, 222 frames,
  **0 corrupt**; post-test device healthy (HD/q12/x18 defaults reapplied, heap 3.7 MB).
- **Two "unexplained reboots" during interactive debugging (2026-09-25) —
  root-caused the same day:** both were the stall-watchdog `esp_restart()`
  (`app_main.c`, clients active + >15 s without delivery), triggered by
  (a) `metrics_mark_stream_active()` initializing the delivery clock only once
  per boot, so a connect after ≥15 s idle raced the 1 s watchdog tick, and
  (b) UXGA 1600×1200 @ q4 producing **zero frames for 16 s** after a config
  change (camera-capture stall — still open, needs serial logs, Phase 4/5).
  **Fixes:** delivery clock resets on every session connect; `reset_reason`
  (`esp_reset_reason()`) exposed in `/api/v1/status`; stream handler probes
  client-gone with a non-blocking `recv` every frame (removes stale
  `stream_clients` and the spurious 409 config rejections); the Qt app gained
  auto-reconnect (5 retries, 0.5–5 s backoff) and one silent 409 retry.
  Verified by a full connect/disconnect/config cycle test: **0 reboots,
  0 spurious 409s**, one mid-test remote FIN recovered by the retry logic.
  Serial capture of the UXGA stall itself still pending (adapter unplugged).

### Harness/firmware fixes made during this session (recorded for honesty)

- Firmware CRC used `esp_rom_crc32_le(0xffffffff,…)^0xffffffff`, which is **not**
  zlib-compatible (the ROM API sandwiches `~` before and after); verified against
  IDF's own vector and a live frame pair → fixed to `esp_rom_crc32_le(0,…)`
  (== `zlib.crc32`), re-flashed, TCP/UDP CRC failures now 0. Protocol v1 states
  CRC32 = zlib-compatible.
- UDP drop counter in the harness double-counted losses and assumed seq starts at 0
  (inflated results up to 24.9 M — impossible values); fixed to a single
  end-of-run computation from per-segment min/max; the two invalid runs' JSON files
  were deleted rather than presented as data.
- Ad-hoc debug one-liners (not the harness) discarded TCP read-ahead bytes and
  produced garbage "headers" — session-length stalls were client artifacts; the
  harness's persistent-buffer parser was unaffected.

## Phase 4 — camera matrix (2026-09-26)

- **Method:** `benchmarks/phase4_matrix.py` (HTTP MJPEG receive-only + `/api/v1/status`
  polled at 1 Hz), three stages per `benchmark-plan.md`: **envelope** (7 resolutions ×
  4 qualities = 28 cells at baseline fb3/latest/psram/xclk 18 MHz, 55 s + 10 s warm-up),
  **fbgrab** (fb_count 1–3 × grab latest/cont × fbloc psram/dram = 12 cells at the
  boundary point, 55 s + 10 s), **confirm** (ladder candidates + first failing point,
  130 s = 120 s measured + 10 s warm-up). XCLK 18 MHz throughout (Phase 2 clean clock).
- **Environment:** same softAP/PC as Phase 3 (RSSI −33…−36 dBm); **daylight session** —
  scene detail changes JPEG sizes at a fixed quality, see drift note below.
- **Raw data:** `benchmarks/results/phase4-20260925-fixed.jsonl` (one removed entry
  archived in `…fixed.removed.jsonl`, see incidents). Firmware `741eeb8-dirty`
  (Phase 4 config API: `fb_count`/`grab`/`fbloc` params, atomic apply + rollback,
  on-demand sensor recovery; status adds `grab_mode`, `fb_location`, `reset_reason`).

### Envelope (fb3 / latest / psram / xclk 18 MHz, 55 s per cell)

Mean delivered FPS; ≥15 floor (plan) marked ✓:

| Resolution | q4 (highest) | q12 (high) | q24 (balanced) | q36 (performance) |
|---|---|---|---|---|
| UXGA 1600×1200 | 1.04 | 3.82 | 7.45 | 9.81 |
| SXGA 1280×1024 | 2.85 | 6.39 | 11.22 | 11.26 |
| HD 1280×720 | 4.05 | 8.96 | 11.25 | 11.18 |
| XGA 1024×768 | 4.90 | 11.25 | 11.13 | 11.25 |
| SVGA 800×600 | 7.71 | **19.65 ✓** | **22.50 ✓** | **22.51 ✓** |
| VGA 640×480 | 12.92 | **22.49 ✓** | **22.50 ✓** | **22.03 ✓** |
| QVGA 320×240 | **24.20 ✓** | **44.95 ✓** | **45.00 ✓** | **45.01 ✓** |

- SXGA/HD/XGA plateau at ~11.2 fps regardless of quality — softAP/Wi-Fi byte-rate
  cap at those sizes, not sensor or encode limits.
- Boundary point derived by the harness: **svga/q12** (largest pixels meeting the
  ≥15 floor at the highest quality).

### fb_count × grab × fbloc cross (at svga/q12, 55 s each)

| Config | FPS | p50 bytes |
|---|---|---|
| fb1 / latest / psram | 7.32 | 27847 |
| fb1 / cont / psram | 7.61 | 25800 |
| **fb2 / latest / psram** | **15.91** | 28089 |
| fb2 / cont / psram | 14.37 | 28208 |
| fb3 / latest / psram | 11.44 | 34619 |
| fb3 / cont / psram | 11.37 | 35663 |

- Winner: **fb2 / latest / psram** — fb1 starves the grab path (~7 fps), fb3 adds
  transport latency/overhead (~11 fps); cont-grab is consistently slightly slower
  than latest.
- **DRAM fbloc is infeasible at SVGA and above** (96000 B/buffer > largest internal
  RAM block): recorded as `ESP_ERR_NO_MEM` rejections; DRAM fbloc remains usable at
  QVGA (320×240 → 15360 B/buffer, measured working).

### Confirm ladder (fb2 / latest / psram / xclk 18 MHz, 130 s per point)

Ordered best-quality-first operating points meeting the ≥15 fps floor:

| # | Config | FPS mean | FPS p50/s | p50 bytes | p95 interval | ≥20 |
|---|---|---|---|---|---|---|
| 1 | svga/q36 | 16.34 | 16 | 18275 | 103.45 ms | no |
| 2 | vga/q24 | 18.97 | 19 | 14680 | 94.22 ms | no |
| 3 | vga/q36 | 19.21 | 20 | 12294 | 93.46 ms | no |
| 4 | qvga/q12 | 42.00 | 42 | 8086 | 40.79 ms | yes |
| 5 | qvga/q24 | 44.61 | 45 | 5095 | 36.62 ms | yes |
| 6 | qvga/q36 | 44.93 | 45 | 4069 | 40.24 ms | yes |

Below the floor (measured, excluded from ladder): svga/q12 9.95, svga/q24 14.37,
vga/q12 14.87, uxga/q4 2.16 (fb2), qvga/q4 = dead cell (see incidents).

**Same-point drift (honesty note):** svga/q12/fb2/latest measured 19.65 (envelope,
55 s), 15.91 (fbgrab, 55 s), 9.95 (confirm, 130 s) across the session — p50 frame
size grew 28089 → 36379 B as the daylight scene changed, i.e. absolute FPS at a
fixed quality is scene-dependent; ladder *ordering* holds within each stage but
per-point values carry this uncertainty.

### Findings & incidents (recorded, not hidden)

- **QVGA@q4 fb-overflow:** the JPEG frame buffer for QVGA is 15360 B
  (`w*h/5`, `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_AUTO=y`); q4 frames on a detailed
  daylight scene sit right at the cap → `cam_hal: FB-OVF` storm (frame drops in the
  ISR path). In confirm it collapsed delivery to 0.51 fps (first run, full 130 s,
  no reboot) and then stalled >15 s → **stall-watchdog reboot** (second run,
  recorded `dead=True`, `events=['reboot','stall']`, serial:
  `stream stalled for 15503 ms with active client, restarting`). QVGA q12–q36
  (4–8 KB frames) are unaffected (42–45 fps). QVGA@q4 is therefore **not a
  usable operating point on detailed scenes**.
- **14 brownout reboots (run 4, hardware):** serial showed
  `E BOD: Brownout detector was triggered` ×14 between confirm cells, which
  invalidated run 4's cells 8–11 (`wait_idle` timed out against a rebooting
  device; misreported as "stream client busy"). No firmware fault — supply-voltage
  dips on the USB/serial path; the loop stopped by itself and all 5 cells were
  re-run cleanly in run 5. **Action: check cable/port/power before Phase 5 soaks.**
- **SCCB wedge → on-demand recovery (root-caused + fixed):** repeated
  deinit/init cycles (DRAM-fail churn) locked the SCCB bus; probe returned
  `ESP_ERR_NOT_SUPPORTED` and the camera reported `no sensor` until a power
  cycle. Fixed in firmware: PWDN power-cycle + 9-clock bus recovery before every
  init, 3 escalating retry rounds for probe-type errors, atomic apply with
  rollback, and the config endpoint now self-repairs a dead camera instead of
  returning 500. DRAM-infeasible configs are rejected **before** touching the
  camera (`ESP_ERR_NO_MEM` pre-check) so they cannot churn the bus. Harness
  recovery (`recover_dead_camera`) verifies health by snapshot instead of
  assuming a reboot happened. Zero wedges in run 5.
- **DRAM fbloc at SVGA+** fails at `frame buffer malloc failed` inside the driver
  when no pre-check existed; now rejected cleanly with rollback proven intact
  (`camera apply failed (ESP_FAIL), rollback ok` records kept).

## Phase 4b — viewer reconnect failure: root cause and fix (2026-09-26)

Symptom: connect → disconnect → change resolution → connect would fail after 2–3
cycles with "no response from camera" and endless `409 stream active`.

### Root cause (all items measured, none inferred)

1. **Frame-buffer overflow (primary).** The driver sized JPEG buffers at
   `w*h/5` (AUTO), which the Phase-4 data already showed to be 71–99% full at
   quality 4. At the quality the viewer was using (SVGA, **q2**) a detailed scene
   exceeds it → `cam_hal: FB-OVF` storm, then `esp_camera_fb_get()` returns only
   NULL after its ~4.5 s timeout, forever. Serial during the failing session:
   hundreds of `FB-OVF` lines and `Failed to get frame: timeout` every ~4.5 s
   with `frames_captured` frozen.
2. **225 s handler lockup.** `consecutive_fails > 50` × 4.5 s kept the
   single-threaded stream httpd task inside one handler, so no new connection was
   served and `stream_clients` never returned to 0 → every config got 409.
3. **Watchdog blind spot.** `stall_watchdog_task` only ran while a client was
   attached; once the viewer gave up, the broken camera stayed broken and the
   next connect could never succeed.
4. **Reinit storm.** Every config change, including each quality-slider movement,
   did a full deinit/init.
5. Socket-pool exhaustion (`accept (23)` = ENFILE, 14 sockets needed vs
   `CONFIG_LWIP_MAX_SOCKETS=10`) — fixed earlier the same day by raising the pool
   to 16, gating the unused raw transport behind
   `CONFIG_SORTING_CAM_FRAME_TRANSPORT` (default off) and shortening
   `TCP_FIN_WAIT_TIMEOUT` to 5 s.

### Quality-floor calibration (new measurements)

10 s streams, 44–144 frames per cell, per-part `Content-Length` parsed for the
frame maximum; data in `benchmarks/results/qlow-20260926.json` and
`qfloor-20260926.json`.

| res | floor | max frame at floor | fill of 256 KiB budget | fps at floor |
|---|---|---|---|---|
| QVGA | q0 | 59,976 B | 23% | 10.3 |
| VGA | q2 | 105,952 B | 40% | 6.0 |
| SVGA | q2 | 155,955 B | 57–60% | 1.5–5.5 |
| XGA | q6 | 143,673 B | 55–57% | 5.0–6.2 |
| HD | q6 | 167,811 B | 64–70% | 3.5–4.9 |
| SXGA | q8 | 201,317 B | 77–83% | 3.0–3.9 |
| UXGA | q12 → **q16** | 98,587 B (q16) | 38% | 5.6 |

- A linear size-vs-quality model was implemented first and **discarded**: it
  predicted 72,785 B for SVGA q0 where the real frame was ~116 KB at q2, because
  frame size grows steeply and non-linearly below q4.
- Frame size is **scene-dependent by up to ~2x**: UXGA q12 measured 128,659 B
  over 10 s and 259,471 B (99% of budget) in a later run. UXGA's floor was
  therefore raised q12 → q16, and any frame ≥90% of budget is logged.
- Budget choice: 256 KiB, not 512 KiB. 512 KiB pushed the driver's DMA node count
  23 → 128, cost 1.28 MB internal DRAM (free heap 4.0 → 2.7 MB) and coincided
  with brownout resets under maximum-size frames.

### Post-fix verification (firmware with measured floors, build of 2026-09-26)

- Floors enforced: 8/8 negative cases rejected with 400 naming the floor; all
  at-floor values accepted.
- 10/10 connect → disconnect → change resolution → connect cycles streamed on
  both sides of every change.
- 20/20 rapid connect/abort cycles streamed; stream recovered immediately after.
- Quality-only change applied without reinit (`camera quality set to 20 (no reinit)`).
- End state: `camera_recoveries=0, capture_failures=0`, free heap 3.52 MB, free
  PSRAM 3.40 MB of the 4 MB this chip maps.
- Recovery path proven in the wild during calibration: three automatic
  `camera recovered (attempt n)` events after real overflows, and one
  recovery-storm-then-reboot at the (now-forbidden) UXGA q8 cell.
- **Hardware finding:** brownout resets (`E BOD`) continue under heavy streaming
  with maximum-size frames — the USB supply, not the firmware. Streaming large
  frames at high duty cycle is the worst case seen so far.

## Phase 5 — pipeline optimization

### Step 0: PC-side baseline (2026-09-27)

Phase 3 measured the harness with no JPEG decode, so the §13 PC-side metrics
(decode time, render time, UI FPS, end-to-end software latency) had never been
recorded. They are now measured in the app itself:

- `desktop/src/AppMetrics.{h,cpp}` — measurement only, no behaviour change:
  decode/parse/render/scale microseconds, present age and render age
  milliseconds, part/frame/byte counters, process CPU%, working set.
- `--bench <sec> --warmup <sec> --out <file> [--framesize k] [--quality n]` runs
  the real viewer path, resets the counters when the **first frame** arrives, and
  writes JSON on exit.
- `benchmarks/phase5_pipeline.py` launches it, polls `/api/v1/status` every 2 s,
  and **slices the device series to the app's exact measurement window**
  (`metrics_start_ms`/`metrics_end_ms`) so device and PC fps are comparable.

Method: 120 s measurement after a 10 s warm-up that starts at the first decoded
frame, softAP ch1, RSSI −17/−18, fb3/latest/psram, xclk 18 MHz, PC i3-1215U,
Windows 11, Qt 6.11.2. Raw: `benchmarks/results/phase5-baseline-20260927-*.json`.

| run | device fps | app fps | presented fps | decode p50/p95 (µs) | present age p50/p95 (ms) | render p50 (µs) | CPU | MB/s | drops | recoveries | reset |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1280×720 q12 | 8.413 | 8.38 | 8.38 | 9677 / 13331 | 10 / 14 | 1 | 19.3% | 3.80 | 0 | 0 | none |
| 800×600 q36 | 22.279 | 22.28 | 22.28 | 4520 / 6691 | 5 / 7 | 1 | 25.7% | 2.96 | 0 | 0 | none |
| 640×480 q36 | 22.506 | 22.51 | 22.51 | 3288 / 4986 | 3 / 5 | 1 | 22.1% | 2.03 | 0 | 0 | none |
| 320×240 q24 | 44.936 | 44.95 | 44.95 | 931 / 1624 | 1 / 2 | 1 | 24.6% | 1.87 | 0 | 0 | none |

Device-side health during all four runs: `capture_failures` delta 0,
`camera_recoveries` delta 0, `reset_reason` constant (1 = SW_RESET from the
flash, i.e. no reboot), minimum free heap 3.50 MB. No serial log was captured
for these runs (the COM10 reader lost the port to a USB re-enumeration), so the
device-side evidence is the status polling only — stated rather than implied.

### Finding: the camera is the ceiling, not the PC pipeline

In every run `app fps == device fps` within 0.3%, with **zero** dropped, stale or
overwritten frames, 1–14 ms of end-to-end PC age, and 19–31% of one core. Even
at the device's maximum measured rate (44.9 fps) the app presents 44.95 fps with
1–2 ms age. Consequences for the Phase 5 work list:

1. **PC-pipeline optimization cannot raise fps.** Items 1–6 of the plan (decode
   thread, copy reduction, render path, freshness, buffers, synchronization) are
   therefore latency- and CPU-only wins, not fps wins. Decode is 0.93 ms at
   320×240 and 9.7 ms at 1280×720, i.e. ~8% of a core at 8.4 fps and ~4% at
   45 fps — never the limiter at any rate the camera can produce.
2. **The ≥15 fps gate is a device-configuration question.** 1280×720 q12 — the
   shipped default — produces **8.4 fps**, less than half the floor, while the PC
   has 90% of its capacity idle. The device levers are XCLK (Phase 3 measured
   ~27 MHz needed for a clean 15 fps at HD; the clean range tops out at 27),
   `fb_count`/`grab_mode` (Phase 4 found fb2/latest the winner, but these runs
   used the NVS-restored fb3), and resolution/quality.
3. **The render path is not doing what the plan assumed.** `Main.qml` uses
   `fillMode: Image.PreserveAspectFit`, so the scene graph scales the texture and
   the provider is asked for the natural size: `requestImage` measures **1 µs**
   and `scaled()` is never called (`scale_us` sample count 0). The real per-frame
   render cost is the scene-graph texture upload, which `requestImage` does not
   capture. The plan's "per-request CPU resample" claim was wrong; recorded here
   rather than quietly dropped.
4. Latency is already small (3–14 ms PC-side) and the only remaining latency
   lever of size is the deferred early-send-during-DMA work (driver-level, risky,
   documented as deferred).


### Device-side A/B and sweep (2026-09-27, same 120 s + 10 s warm-up method)

Run with the app in bench mode so device and PC metrics cover the identical
window; every run is checked for config mismatch, device reboot and capture
failures before its numbers are used. Bench-only knobs were added for this
(`--fb-count`, `--grab`, `--xclk`); no product behaviour changed.

**fb2/latest vs fb3/latest (XCLK 18 MHz)** — Phase 4 had recorded fb2/latest as
the winner, so this was the first thing to re-test:

| point | fb3 device fps | fb2 device fps | delta | fb2 run |
|---|---|---|---|---|
| 1280×720 q12 | 8.41 | 7.13 | **−15.2%** | clean |
| 800×600 q36 | 22.28 | 20.64 | **−7.4%** | clean |
| 640×480 q36 | 22.51 | 22.34 | −0.7% | clean |
| 320×240 q24 | 44.94 | 44.25 | −1.5% | clean |

**fb2 does not win at any of these points.** The Phase-4 fb2 advantage was
measured at SVGA q12 (15.91 vs 11.44 fps) — a lower-rate regime where an extra
buffer is not needed. In the high-rate regime the extra buffer is worth 7–15%.
ADR-0008's fb2 recommendation is therefore **not** carried forward as a blanket
rule; fb3/latest is the better default at every point measured today.

**XCLK sweep, SVGA 800×600 q36, fb3/latest:**

| XCLK (MHz) | device fps | app fps | frame B p50 | capture ms p50 | CPU | MB/s | valid |
|---|---|---|---|---|---|---|---|
| 18 | 22.42 | 22.43 | 17,067 | 13.6 | 34.5% | 3.08 | yes |
| 20 | 18.61 | 18.62 | 16,770 | 0.7 | 23.4% | 2.51 | yes |
| 22 | 27.53 | 27.54 | 16,374 | 13.5 | 32.4% | 3.63 | yes |
| 24 | 29.82 | 29.84 | 16,165 | 10.3 | 44.0% | 3.88 | yes |
| 26 | 32.43 | 32.41 | 15,785 | 9.7 | 44.6% | 4.12 | yes |
| 27 | **33.62** | 33.65 | 15,749 | 7.7 | 36.0% | 4.27 | yes |

**XCLK is not monotonic** — 20 MHz is a reproducible local *minimum* (18.6 fps,
below 18 MHz), so a linear "more clock is better" model is wrong. From 22 MHz
the curve climbs steeply and is **still rising at 27 MHz**, which is where the
firmware's validation stops (`xclk` accepted range 6–27). 27 MHz is therefore a
**policy cap, not a hardware limit**, and the next step is to test above it
before treating 27 as the ceiling.

**XCLK sweep, HD 1280×720 q12, fb3/latest:**

| XCLK (MHz) | device fps | app fps | frame B p50 | capture ms p50 | recoveries | valid |
|---|---|---|---|---|---|---|
| 18 | 7.74 | 7.78 | 58,321 | 0.4 | 0 | yes |
| 20 | 5.88 | 5.88 | 57,777 | 0.4 | 0 | yes |
| 22 | 2.16 | 2.16 | 245,849 | 129.5 | 0 | yes (degraded) |
| 24 | 0.00 | 0.00 | 249,514 | 173.2 | 18 | **no frames** |
| 26 | 0.00 | 0.00 | 249,514 | 173.2 | 20 | **no frames** |
| 27 | 0.00 | 0.00 | 249,514 | 173.2 | 18 | **no frames** |

HD behaves in the opposite direction: 20 MHz is already worse than 18, 22 MHz
inflates the frame from 58 KB to 246 KB and collapses to 2.2 fps, and 24 MHz and
above stop producing frames entirely. The recovery path fired 18–20 times per run
and could not recover it — the setting is invalid, not transient. **The XCLK
sweet spot is resolution-dependent**, so a single global clock (as the app sends
today) is the wrong model; clock has to travel with the resolution profile.

### Consequence: HD/q12 is an experimental profile, not the production default

HD/q12 was the shipped default on the strength of being "HD, clean sensor noise,
18 MHz". Measured on 2026-09-27 it delivers **7.7–8.4 fps**, less than half the
≥15 fps floor, and raising its clock makes it worse rather than better. It is
retained as an **experimental profile** (highest resolution, sensor-noise-clean,
useful for stills and snapshot work) and explicitly **not** as the production
default. The production default is not yet chosen: the best measured point today
is SVGA 800×600 q36 at 27 MHz with **33.6 fps** (+50% over 18 MHz), which clears
the floor with margin, at the cost of the XCLK policy cap and the fact that 27 MHz
is untested above the cap. Choosing it is the next decision, not this phase.

### Device-side optimization: camera task affinity (2026-09-27)

`CAMERA_TASK_PINNED_TO_CORE` was already the ESP-IDF default (`core0`), and the
boot log shows the Wi-Fi driver task also on core 0, so moving the camera task
off core 0 was the plausible win. All three variants, HD/q12/x18/fb3/latest,
120 s + 10 s warm-up, one flash per variant:

| camera task | device fps | frame B p50 | PC age p50/p95 (ms) | CPU | vs core0 |
|---|---|---|---|---|---|
| **core0** (IDF default) | **11.01** | 49,507 | 14 / 20 | 33.0% | — |
| core1 | 8.53 | 50,020 | 16 / 23 | 27.7% | **−22.5%** |
| no_affinity | 9.18 | 49,219 | 12 / 18 | 25.9% | **−16.7%** |

**core0 wins; both alternatives are reverted** per the Phase 5 rule that anything
which does not win on a real metric goes back. Frame sizes agree within 1.2%
across the three runs, so the scene was stable and the comparison is not
confounded by the scene sensitivity seen elsewhere in this document.
`no_affinity` does give the best latency (12 ms p50 against 14 ms) — the only
metric where it wins — but it costs 17% of the frame rate, and frame rate
outranks latency in the project priority.

Caveat stated rather than hidden: **one run per variant.** The frame-size
agreement is the strongest available control on scene variation, but the
8.5–11.0 spread is wider than a repeat would ideally settle. A second core0 run
is the cheap way to tighten this if the number ever matters for a decision.

### Optimization deltas

(Baseline vs each isolated change; link ADRs in `docs/decisions/`. Nothing
measured yet - the Step 0 finding above re-prioritized the work list.)

## Soak tests

(≥1 h runs: memory trend, leak/fragmentation/stall findings.)
