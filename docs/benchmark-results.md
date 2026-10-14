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

Median (p50) frame size in bytes for the same cells. ADR-0012 requires every
figure the ladder publishes to be recorded here, and `ProfileEngine` cites this
column alongside the FPS column above; both are read from
`benchmarks/results/phase4-20260925-fixed.jsonl`:

| Resolution | q4 (highest) | q12 (high) | q24 (balanced) | q36 (performance) |
|---|---|---|---|---|
| UXGA 1600×1200 | 343,758 | 129,856 | 71,173 | 56,409 |
| SXGA 1280×1024 | 218,376 | 80,596 | 46,929 | 38,463 |
| HD 1280×720 | 151,830 | **57,317** | 34,045 | 27,761 |
| XGA 1024×768 | 124,485 | 43,828 | 26,746 | 22,780 |
| SVGA 800×600 | 67,992 | 26,737 | **16,982** | 14,236 |
| VGA 640×480 | 44,253 | 19,180 | 11,315 | 9,608 |
| QVGA 320×240 | 15,175 | **7,119** | 4,079 | **4,066** |

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

Below the floor (measured, excluded from ladder), with the same p50 byte figure
so every confirm cell carries both numbers: svga/q12 9.95 / 36,379 B, svga/q24
14.37 / 23,077 B, vga/q12 14.87 / 23,083 B, uxga/q4 2.16 / 241,153 B (all fb2),
qvga/q4 = dead cell (see incidents).

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

| run | device fps | app fps | presented fps | decode p50/p95 (µs) | present age p50/p95 (ms) | render p50 (µs) | CPU | Mbps | drops | recoveries | reset |
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

| XCLK (MHz) | device fps | app fps | frame B p50 | capture ms p50 | CPU | Mbps | valid |
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

### Consequence: HD/q12 is the production default, on a provisional floor

HD/q12 was the shipped default on the strength of being "HD, clean sensor noise,
18 MHz". Measured on 2026-09-27 it delivers **7.7–8.4 fps**, less than half the
≥15 fps floor, and raising its clock makes it worse rather than better.

The owner chose it anyway on 2026-09-27 (ADR-0010, decision 1): image quality
outranks frame rate in this project's priority order, so the highest-resolution
point with sensor-noise-clean output is the production default *even though it
does not clear the general floor*. Its floor is therefore **≥7 fps and
provisional** — ADR-0010 decision 2 scopes it that way and decision 3 requires
the measured numbers to be published as they are. The ≥1 h soak of D1 is what
replaces the provisional ≥7 with a sustained figure; until it runs, 7.0 is an
owner-decided placeholder rather than a measured requirement, and the desktop
UI marks it as provisional instead of rendering it as a settled target.

The 7.7–8.4 fps measurement stands unchanged, as does its consequence:
**§34's acceptance target is not met at the production default**, and ADR-0010
decision 3 requires that fact — and the hardware reason for it (the camera's
frame-production rate tops out near 11.2 fps at 1280×720, and XCLK is already
at its safe limit) — to stay published rather than quietly dropped. The best
measured point on this gate arithmetic remains SVGA 800×600 q36 at 27 MHz with
**33.6 fps** (+50% over 18 MHz), which clears the floor with margin, at the cost
of the XCLK policy cap and the fact that 27 MHz is untested above the cap.
Whether to switch the default to it is D1's open question, not a settled choice.

### Gate arithmetic (2026-09-27)

§34 asks for "highest practical image quality / sustained minimum 15 FPS /
preferred 20 FPS+", with the exact combination established empirically. Best
**valid** measurement per point, 120 s app-path runs:

| resolution | quality | fps | frame B | B/pixel | KB/s (Mbps) | clears 15 | clears 20 |
|---|---|---|---|---|---|---|---|
| 320×240 | q24 | **44.94** | 5,084 | 0.066 | 229 (1.87) | yes | yes |
| **800×600** | q36 | **33.62** (x27) / 22.42 (x18) | 15,749 | 0.033 | 530 (4.27) | yes | yes |
| 640×480 | q36 | **22.51** | 11,195 | 0.036 | 252 (2.03) | yes | yes |
| 1280×720 | q24 | 11.17 | 30,775 | 0.033 | 344 (2.79) | **no** | no |
| 1280×720 | q12 | 11.01 | 49,507 | 0.054 | 545 (4.36) | **no** | no |

**Correction:** the throughput columns in the earlier tables were labelled MB/s;
they are Mbps. Peak observed throughput is 545 KB/s (4.36 Mbps), not 4.36 MB/s.

**What is actually binding.** Not the link: 4.36 Mbps is a fraction of what a
802.11n 40 MHz softAP link carries with one station, and the device is nowhere
near saturating it. Not the PC: it consumed 100% of delivered frames in every
run with zero drops at 19-36% of one core and 2-23 ms p95 age. **fps is the
camera's frame-production rate** at each (resolution, quality, clock).

**The 1280x720 arithmetic.** Reaching 15 fps at HD needs +34% frame production
over the 11.17 fps best case. The only measured lever is XCLK, and it is already
at its safe limit: 20 MHz is worse than 18, 22 MHz degrades to 2.2 fps with
246 KB frames, 24 MHz and above produce none. Phase 4 measured the same ceiling
independently (HD q24 11.25, q36 11.18 fps, harness, 55 s), so two methods agree:
**HD tops out near 11.2 fps, about 74% of the floor, and no quality value or safe
clock reaches 15.**

**Per-pixel quality is the cleanest way to read the trade.** HD q24 and SVGA q36
both encode 0.033 bytes per pixel - the same per-pixel image quality - but SVGA
runs at 33.6 fps against HD's 11.2. HD q12 is 0.054 B/px, 1.6x the per-pixel
quality, at the same ~11 fps. So the real choice is not "HD or SVGA" but
"three times the frame rate, or the same frame rate at 2.25x the pixels".

**Largest resolution clearing the preferred 20 fps target: 800x600.** The
production resolution remains the project owner's decision (1280x720, set
2026-09-27); what the measurements settle is that the >=15 fps acceptance cannot
be met at 1280x720 on this hardware, and is met with margin from 800x600 down.

### Partial soak at the production default (2026-09-27, 4.9 min — not the 1 h gate)

HD/q12/x18/fb3/latest, stopped early at the project owner's request. Device-side
record only (the app was terminated, so no PC metrics for this window):

| metric | value |
|---|---|
| window / samples | 293 s, 140 status polls at 2 s |
| uptime | 2325 s → 2618 s, **monotonic, no reboot** (`reset_reason` constant 1) |
| capture / delivery fps | 8.08 / 8.08 |
| frames | 2,368 captured, 2,367 delivered (1 in flight at cutoff) |
| capture failures | **0** (delta and max) |
| camera recoveries | **0** (delta and max) |
| free heap | 3,516,724 → 3,502,896 B (**−13.8 KB**, full observed span 17 KB) |
| free PSRAM | +144 B (flat) |
| RSSI | −23 … −16 dB (7 dB span) |
| brownouts | none in this window |

Healthy on every stability counter, but **4.9 minutes cannot certify long-duration
stability** and this is recorded as a partial run, not as the §34 gate. The heap
drift is inside the observed noise band (17 KB span), so a longer run is needed to
separate drift from noise — the ≥1 h soak remains open.

### Why HD measures 11–12 fps sometimes and 7–8 fps other times

Same configuration, seven runs, HD/q12/x18/fb3/latest:

| run | fps | frame B | frame interval | ms per KiB of frame | avg_capture_ms |
|---|---|---|---|---|---|
| affinity core0 | 11.01 | 49,507 | 90.8 ms | 1,878 | 6.1 |
| hddef q12 | 10.46 | 45,455 | 95.6 ms | 2,154 | 2.7 |
| affinity no_affinity | 9.18 | 49,219 | 109.0 ms | 2,267 | 0.4 |
| affinity core1 | 8.53 | 50,020 | 117.2 ms | 2,399 | 0.4 |
| baseline | 8.41 | 56,565 | 118.9 ms | 2,152 | 0.6 |
| xclk sweep | 7.74 | 58,321 | 129.1 ms | 2,267 | 0.4 |
| fb2 | 7.13 | 60,943 | 140.2 ms | 2,355 | 22.4 |
| partial soak | 8.08 | 58,512 | 123.8 ms | 2,217 | — |

**Correlation between frame size and fps: −0.852**, and the frame interval is
almost perfectly linear in the compressed frame size at **~2.2 ms per KiB**
(spread 1,878–2,399, ±6% around the mean) with `avg_capture_ms` at 0.4–6 ms.

So nothing regressed and no setting changed. The mechanism is:

1. At a fixed JPEG quality the **compressed size tracks scene complexity** — more
   detail or motion in frame means a bigger file for the same quality number.
2. The OV2640 encodes in the sensor, and encode time scales with the data it has
   to emit, so bigger frames take longer. The driver paces frames on the sensor's
   end-of-frame, which is why `camera_fb_get` returns almost instantly
   (0.4–6 ms) yet frames only arrive every 90–140 ms.
3. The **11–12 fps observations are the ones where frames were small enough**
   (45–50 KB) that encode time was not yet the binding constraint. Above roughly
   55 KB the encode interval (≈130 ms) caps the rate at ~7.7 fps.

There is also a hard ceiling near **11.2 fps at 1280×720**, visible as the maximum
across unrelated configurations — HD q24 11.17, q12 11.01, and Phase 4's harness
runs at q24 11.25 / q36 11.18. At low quality numbers (bigger frames) the encode
time exceeds that ceiling and becomes the limit instead.

Consequences, all already reflected in the documentation: the HD floor is a band
(≥7 provisional) rather than a single number, §34's ban on quoting an
undemonstrated fps figure applies with extra force here, and the frame rate a user
sees depends on what the camera is pointed at. SVGA and below sit far enough below
the encode-bound region (16 KB frames → ~35 ms) that their rates are stable.

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

(Baseline vs each isolated change; link ADRs in `docs/decisions/`. Done:
camera task affinity - core0 kept, core1 and no_affinity reverted and reflashed.
Not done, with the reason: Wi-Fi/LwIP knobs - the measurements show the link is
not the constraint (peak 4.36 Mbps observed against a link that carries far
more) and ESP-IDF's httpd already sets TCP_NODELAY around every chunk send
(`httpd_txrx.c:494`) and disables it afterwards, so the Nagle lever is already
taken. There is no fps headroom for those knobs to win, and latency is already
1-23 ms p95. Re-open only if the link ever becomes the constraint.)

**Re-opened 2026-10-04 as an unmeasured trial** (project owner's request; ADR-0018):
`sdkconfig.defaults` now carries a CPU 240 MHz / flash QIO 80 MHz / lwIP 32 KiB windows /
Wi-Fi dynamic buffers 64 / lwIP IRAM block. No result exists yet. The record above was taken at
160 MHz / DIO 40 MHz / default buffers and does not apply to the new configuration. ADR-0018
holds the A/B protocol and the rule to revert the block if it does not win a real metric.

## Phase 6 — HD/q12 re-measured against the scene (2026-09-29)

The project owner reported that the production default had dropped from
10–11 fps to 4–5 fps. This section records what was measured in response. It
does **not** reproduce the 4–5 figure, and the difference between the two
regimes below is the finding.

### Setup — identical for every row in this section

| field | value |
|---|---|
| firmware | `v0.1.0-7-ga376882`, unchanged for the whole session |
| application | 0.2.0, `desktop/build/SortingCamera.exe` |
| resolution | 1280×720 (`hd`) |
| JPEG quality | 12 (device floor 6) |
| XCLK | 18 MHz (`xclk_max_mhz` 20) |
| frame buffers / grab mode | 3 / `latest` |
| frame budget / fb location | 262,144 B / `psram` |
| PC | i3-1215U / Intel AX201, joined the softAP, RSSI −24 … −26 dBm |
| method | bench mode: warm-up, then a steady window. `frames_overwritten = 0` and `stale_dropped = 0` in every run |

Those 18 MHz / fb3 / latest values are simultaneously the firmware boot
defaults (`app.h:28-32`) and the High Quality profile
(`ProfileEngine.cpp:57-65`), so at this operating point the configuration
cannot drift away from what the UI names.

### Measured — 2026-09-29, evening

| run | window | fps | frame B | recv Mbps | age p50/p95 (ms) | camera `avg_capture_ms` |
|---|---|---|---|---|---|---|
| bench, 8 s warm-up | 20 s | **8.04** | 59,100 | 3.80 | 16 / 20 | 0.618 |
| bench, 2 s warm-up | 40 s | **7.80** | 59,792 | 3.73 | 17 / 22 | 0.283 |
| bench, 2 s warm-up | 40 s | **8.25** | 59,762 | 3.94 | 17 / 27 | 0.434 |
| raw `curl` :81, no application running | 45 s | **7.99** | 55,792 | — | — | — |

The `curl` run's 360 frames ranged min 48,558 / p10 54,346 / **p50 56,545** /
p90 56,825 / max 62,781 B, and each of ten consecutive 10-second segments
landed between 7.99 and 8.56 fps on the model below. The frame rate was not
momentarily dipping; it was flat.

Earlier the same day, same configuration, a less detailed scene:

| run | window | fps | frame B |
|---|---|---|---|
| raw `curl` :81 | 16.1 s | 11.14 | 27,971 |
| bench | — | 11.25 | 30,023 |
| bench | — | 11.26 | 29,520 |
| bench | — | 11.26 | 28,291 |

and Phase 6's D2 baseline the previous evening
(`phase6-baseline-20260928-hd-q12-x18.json`) recorded 9.96 / 9.97 / 10.00 fps
at 28,956 B per frame. Raw artifacts for the three bench runs are
`phase6-recheck-20260929-hd-q12-x18-warm8-20s.json`,
`phase6-recheck-20260929-hd-q12-x18-warm2-40s-a.json` and
`phase6-recheck-20260929-hd-q12-x18-warm2-40s-b.json` in
`benchmarks/results/`. The 45-second `curl` capture was 20 MB and was not
retained; its figures come from parsing the `Content-Length` header of each
MJPEG part.

### Measured — the camera produces the limit, not the application

The bracketed 42-second streaming window between two device status snapshots
is the decisive measurement. The camera counts its own frames, so this
compares the sensor against the screen without trusting the app's own counter:

| quantity | value |
|---|---|
| camera frames captured | 344 over 42.0 s = **8.19 frames/s** |
| camera frames delivered | 343 over 42.0 s = **8.17 frames/s** |
| application frames presented | **8.25 fps** |
| `frames_overwritten` / `stale_dropped` | 0 / 0 |
| `capture_failures` / `camera_recoveries` | 0 / 0 |
| `stream_clients` / `tcp_clients` | 1 / 0 |

Captured, delivered and presented agree to within 1%, so nothing between the
sensor and the screen is dropping frames and nothing between them is the
limit. `avg_capture_ms` held at 0.28–0.62 ms throughout, meaning
`camera_fb_get()` returned a frame that was already waiting: the sensor was
not blocked on exposure, on the driver, or on a missing buffer — it was simply
producing each frame slowly. Raw `curl` with no application running at all
measures the same 7.99 fps, which reaches the same conclusion from the
opposite direction.

Ruled out by measurement, not by argument: the link (3.80–3.94 Mbps today,
against the 4.36 Mbps peak already recorded in this document); a second
client (an ad-hoc 2026-09-28 probe ran two at once and both held 7.40 and
8.12 fps, 7.16 Mbps combined, with no per-client loss); decode and paint cost
(`decode_us` p95 19.6–25.9 ms is measured *inside* the 121–128 ms frame
budget, and opening the settings panel costs 0 fps for +6.7% CPU and +95 MB
working set); and configuration drift —
`resolution`, `quality`, `xclk_mhz`, `fb_count` and `grab_mode` were read back
from the device in every run and never differed from the table above.

### Interpretation — frame size against frame rate (correlation, not a law)

Dividing each run's frame interval by its frame size in KiB separates the two
regimes cleanly:

| regime | frame size | frame interval | implied ms per KiB |
|---|---|---|---|
| smaller frames | 27.3 – 28.8 KiB | 88.8 – 89.8 ms | 3.03 – 3.29 |
| larger frames | 54.5 – 58.4 KiB | 121.2 – 128.2 ms | 2.08 – 2.30 |

In the large-frame regime the frame interval is linear in compressed size at
**~2.2 ms per KiB**, reproducing the relation already documented above
(correlation −0.852, per-run spread 1,878–2,399). In the small-frame regime
the implied cost per KiB *rises* to ~3.0–3.3 and the interval stops improving:
at ~29 KB, frame size is no longer what sets the rate, and the largest value
ever observed at this operating point is **11.26 fps**.

**This is why one configuration yields 11 fps and then 8 fps on the same
day.** At a fixed JPEG quality number, compressed size tracks how much
detail and motion the frame contains, and the OV2640 encodes inside the
sensor, so a larger frame takes longer to produce. Two `curl` runs on
2026-09-29, one at 27,971 B and one at 55,792 B, differ by 29% in frame rate
for that reason alone, with nothing reconfigured between them.

### Hypothesis — and what is deliberately not claimed

- **The 4–5 fps report is not measured and was not reproduced.** Every
  large-frame run above clusters within a few percent of the ~2.2 ms/KiB line,
  and the lowest figure recorded today is 7.80 fps. Reaching 4–5 fps under
  that same relation would require frames of roughly 90–110 KiB, about twice
  today's. **No run in this repository has recorded a frame that large at
  HD/q12**, so that frame size is a prediction from the model and not
  evidence. If the figure appears again, the frame byte count in the footer is
  the one number that settles it.
- **One cause that would have produced exactly this, now closed.** Before
  commit `9beced1`, `setConfigQuery()` held a single pending slot and dropped
  every write that arrived while a request was in flight, so applying a
  profile transmitted only the first of its five settings. Choosing High
  Quality could therefore leave the camera at a *lower* JPEG quality number —
  same resolution, same profile name in the UI, roughly double the frame
  bytes — which is the shape of the report above. Fixed and covered by
  `aProfileLeavesAsASingleConfigRequest`.
- **Not claimed: that 8–11 fps is the ceiling of this sensor.** The ~11.2 fps
  figure is the largest value ever *observed* at 1280×720, across unrelated
  configurations (q24 11.17, q12 11.01, harness q24 11.25 / q36 11.18). No run
  has demonstrated a limit, and a higher rate may need only a smaller frame
  than any run so far has produced. Every figure above is a measurement of
  *this* configuration under *these* conditions, not a hardware ceiling.
- **Still open: why the scene changed.** Nothing in the firmware, the
  application, or the camera's configuration changed between the 11 fps
  window and the 8 fps window on 2026-09-29; the camera was simply pointed at
  whatever was in front of it. The missing evidence is a pair of runs with the
  scene deliberately held still and then deliberately changed, which is the
  controlled version of what was observed by accident here.

## Soak tests

**DEFERRED — FINAL VALIDATION / PHASE 8.** No long-duration stability claim is
made anywhere in this document. Every result above comes from runs of
**120 s steady-state after a 10 s warm-up**, plus one 4.9 min partial soak and
one aborted attempt — none of which can certify long-duration behaviour.

Two runs are on record and neither counts as a soak:

- `phase5-soak-20260927-hd-q12-20min.json` — device-only, **293 s**, no client.
- `phase6-soak-20260928-hd-q12-ABORTED-145s.json` — client attached, **145 s of
  a planned 3600 s**, terminated externally while concurrent builds/tests were
  running on the same machine. The raw record is kept; it is deliberately
  named ABORTED so it cannot be read as evidence.

The ≥1 h gate in `benchmark-plan.md` remains **open**.

### The final test is not a repeat of this procedure

Phase 8 will be designed around the **actual final architecture** — final
firmware, final Qt application, final networking, final recording path, final
UI — not by replaying the current 60-minute streaming procedure. It must
exercise the behaviours that exist by then and cannot exist yet: resolution
changes, snapshot, record start/stop, PC Wi-Fi leave/rejoin, client
kill/reconnect, camera reboot, and every device-transition the §24 state
machine can enter.

Telemetry required for that test is **preserved, not stripped**: per-sample
device counters (uptime, free heap, free PSRAM, RSSI, capture failures,
recovery count, reset reason), application counters (decoded, dropped, decode
failures, presented fps, thread CPU, peak RSSI age), and the validity report
that refuses a run when any of them is missing.

## Network bench, Phase 2 (camera off)

Every number below comes from `benchmarks/results/phase2-link-baseline-20261009-121152.json`
(NBB, `--label baseline --expect baseline`) and
`benchmarks/results/phase2-link-block-20261009-122431.json`
(NB, `--label block --expect new`), produced by `benchmarks/phase2_link.py` on
2026-10-09. Throughput and per-second percentiles are the median over the three
repetitions of a cell; `stalled s` and `slow_sends` are totals over the three
repetitions; `max_send ms` is the maximum over repetitions; `min free internal`
is the minimum over repetitions.

### Firmware NBB — baseline

| cell | valid reps | PC mean Mbps (median) | per-second p5 | per-second p50 | per-second p95 | stalled s | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int / chunk 8192 | 3 / 3 | 11.141 | 7.306 | 11.119 | 11.646 | 3 | 14 | 127.3 | 144287 |
| int / chunk 1460 | 3 / 3 | 7.502 | 6.624 | 7.717 | 8.129 | 4 | 13 | 563.5 | 144287 |
| int / chunk 32768 | 3 / 3 | 10.510 | 8.711 | 10.661 | 12.122 | 1 | 35 | 395.0 | 110735 |
| psram / chunk 8192 | 3 / 3 | 8.129 | 7.483 | 8.513 | 9.110 | 2 | 8 | 94.6 | 110735 |

INFO (before → after): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) unchanged;
uptime_s 113 → 540.

### Firmware NB — block

| cell | valid reps | PC mean Mbps (median) | per-second p5 | per-second p50 | per-second p95 | stalled s | slow_sends | max_send ms | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| int / chunk 8192 | 3 / 3 | 28.735 | 19.772 | 29.418 | 30.759 | 1 | 10 | 124.5 | 62119 |
| int / chunk 1460 | 3 / 3 | 20.155 | 19.376 | 20.061 | 20.929 | 0 | 0 | 22.0 | 47683 |
| int / chunk 32768 | 3 / 3 | 29.349 | 27.444 | 29.530 | 30.841 | 0 | 0 | 34.3 | 26259 |
| psram / chunk 8192 | 3 / 3 | 24.831 | 22.706 | 24.861 | 26.785 | 0 | 0 | 28.1 | 26259 |

INFO (before → after): cpu_mhz 240; flash_mode_cfg "qio", flash_freq 80m,
runtime.flash_io qio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) unchanged;
uptime_s 101 → 528.

Camera off, 30 s runs, 3 repetitions, all valid, no resets. The block's TCP
throughput is 2.6x to 3.1x the baseline's. Minimum free internal RAM under
saturating load was 110,735 B (baseline) and 26,259 B (block). Nothing here
measures camera fps.

## Network bench, Phase 2 (camera on)

benchmarks/phase2_cam.py in the tree carries a local fix not in patches/phase2-cam-runner.patch: an unauthenticated capabilities probe that answers 401 is treated as 'authentication required' (git hash-object fc7135579033ad5723663f587b5b7b1d3d1b3dc3 after the frame-integrity patch).

Every number below comes from `benchmarks/results/phase2-cam-baseline-20261009-193446.json`
(run 1, NCB baseline), `benchmarks/results/phase2-cam-block-20261009-195017.json`
(run 2, NC block) and `benchmarks/results/phase2-cam-baseline-20261009-202150.json`
(run 3, NCB baseline repeat), produced by `benchmarks/phase2_cam.py` on 2026-10-09,
in run order NCB, NC, NCB. `fps PC` and `fps device` are the medians over the three
repetitions of a profile (PC frame accounting and device INFO counter); frame KiB
p50 / p95 and frame interval p95 are medians over repetitions of the per-repetition
percentiles; `stalled s` and `slow_sends` are totals over the three repetitions;
`min free internal` is the minimum over repetitions.

### Run 1 — baseline (NCB)

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 19.26 | 19.39 | 9.9 / 9.9 | 97.9 | 1.589 | 1 | 385 | 112003 |
| xga q24 @18 | 3 / 3 | 7.33 | 7.53 | 37.2 / 37.2 | 235.9 | 2.296 | 3 | 688 | 112003 |
| sxga q24 @18 | 3 / 3 | 9.00 | 8.95 | 37.2 / 37.2 | 210.1 | 2.728 | 0 | 747 | 111175 |
| uxga q36 @18 | 3 / 3 | 5.85 | 5.96 | 37.2 / 37.2 | 281.0 | 1.820 | 8 | 539 | 111175 |
| uxga q16 @18 | 3 / 3 | 7.19 | 6.98 | 37.2 / 37.2 | 261.9 | 2.131 | 0 | 600 | 111019 |

INFO (before → after): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) unchanged;
uptime_s 115 → 720.

### Run 2 — block (NC)

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 22.48 | 22.57 | 9.9 / 10.0 | 69.5 | 1.817 | 3 | 1 | 54395 |
| xga q24 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 114.8 | 3.453 | 0 | 29 | 40783 |
| sxga q24 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 115.4 | 3.457 | 0 | 29 | 40783 |
| uxga q36 @18 | 3 / 3 | 11.26 | 11.32 | 37.2 / 37.2 | 130.0 | 3.455 | 0 | 110 | 40783 |
| uxga q16 @18 | 3 / 3 | 11.22 | 11.29 | 37.2 / 37.2 | 126.4 | 3.442 | 1 | 260 | 36771 |

INFO (before → after): cpu_mhz 240; flash_mode_cfg "qio", flash_freq 80m,
runtime.flash_io qio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) unchanged;
uptime_s 71 → 670.

### Run 3 — baseline repeat (NCB)

| profile | valid reps | fps PC | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal |
|---|---|---|---|---|---|---|---|---|---|
| svga q12 @18 | 3 / 3 | 22.48 | 22.52 | 9.9 / 10.0 | 66.7 | 1.824 | 0 | 35 | 112907 |
| xga q24 @18 | 3 / 3 | 10.52 | 10.58 | 37.2 / 37.2 | 145.7 | 3.227 | 0 | 906 | 112907 |
| sxga q24 @18 | 3 / 3 | 10.81 | 10.89 | 37.2 / 37.2 | 132.5 | 3.321 | 1 | 888 | 111403 |
| uxga q36 @18 | 3 / 3 | 10.15 | 10.10 | 37.2 / 37.2 | 155.8 | 3.081 | 2 | 894 | 111403 |
| uxga q16 @18 | 3 / 3 | 10.41 | 10.41 | 37.2 / 37.2 | 151.7 | 3.177 | 0 | 911 | 111403 |

INFO (before → after): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) unchanged;
uptime_s 131 → 733.

Camera on, same scene and position, 30 s runs, 3 repetitions, all valid, no resets;
order NCB, NC, NCB. The two baseline runs differ from each other in median fps by
1.8 to 4.3 fps. Minimum free internal RAM during the runs: 111,019 B (baseline) and
36,771 B (block), in a bench build without the MJPEG stream server. Frame size was
37.2 KiB p50 for XGA q24, SXGA q24, UXGA q36 and UXGA q16 alike; explained:
the frames were black (verified from saved samples on 2026-10-10), so these
runs measured trivial frames; see ADR-0019. The camera-off results above are
not affected.

## Camera-on verification with real frames (2026-10-11)

Source: `benchmarks/results/phase2-cam-verify2-20261011-012921.json`
(`--label verify2 --expect baseline`), produced by `benchmarks/phase2_cam.py`
on 2026-10-11. One 10 s run per profile (valid reps 1 / 1 throughout);
`real size` is the dominant frame size read from the JPEG headers.

| profile | real size | frame KiB p50 | fps | link Mbps |
|---|---|---|---|---|
| svga q12 @18 | 800x600 | 16.4 | 9.75 | 1.38 |
| xga q24 @18 | 1024x768 | 20.3 | 8.25 | 1.45 |
| hd q24 @18 | 1280x720 | 22.6 | 6.22 | 1.18 |
| sxga q24 @18 | 1280x1024 | 32.8 | 6.00 | 1.67 |
| uxga q36 @18 | 1600x1200 | 43.6 | 4.25 | 1.43 |
| uxga q16 @18 | 1600x1200 | 55.1 | 2.50 | 1.20 |

Scene: plain wall corner (low detail), baseline firmware (NCB), one 10 s run
per profile. fps was 2.5 to 9.8, far below the black-frame runs, with only
1.2 to 1.7 Mbps of link in use. Exposure is the suspected limit; to be tested
(exposure_sweep.py).

## Exposure sweep (2026-10-12)

Source: `benchmarks/results/exposure-sweep-room-20261012-115542.json` (room)
and `benchmarks/results/exposure-sweep-lamp-20261012-120245.json` (lamp),
produced by `benchmarks/exposure_sweep.py` on 2026-10-12. Baseline firmware
(NCB): cpu 160, flash not-qio/40m/dio. XGA q24 @18, 10 s x 2 reps per cell,
all valid. `longest gap` is the maximum frame interval over repetitions;
`gap p95` the median of the per-repetition p95 intervals; `brightness` the
snapshot luma mean / 255.

Sensor controls before the sweep (identical in both files — restored defaults):
aec 1, aec2 1, aec_value 300, agc 1, agc_gain 0, gainceiling 0, awb 1,
awb_gain 1, bpc/wpc/raw_gma/lenc/dcw 1,
brightness/contrast/saturation/ae_level/special_effect/wb_mode/hmirror/vflip/colorbar 0.

### Room (scene with a bright window)

| profile | set | valid reps | fps | longest gap ms | gap p95 ms | frame KiB | link Mbps | brightness |
|---|---|---|---|---|---|---|---|---|
| xga q24 @18 | auto-default | 2 / 2 | 11.29 | 150.7 | 112.9 | 27.24 | 2.54 | 106.3 |
| xga q24 @18 | auto-gain32x | 2 / 2 | 11.29 | 152.3 | 114.0 | 27.23 | 2.54 | 106.6 |
| xga q24 @18 | auto-gain128x | 2 / 2 | 11.21 | 170.0 | 113.9 | 27.19 | 2.54 | 106.8 |
| xga q24 @18 | manual-e300-g0 | 2 / 2 | 10.93 | 209.3 | 137.3 | 26.07 | 2.39 | 93.0 |
| xga q24 @18 | manual-e600-g10 | 2 / 2 | 11.29 | 130.1 | 107.0 | 19.82 | 1.86 | 242.4 |
| xga q24 @18 | manual-e1000-g20 | 2 / 2 | 11.21 | 121.3 | 106.3 | 15.77 | 1.48 | 253.9 |

INFO (before sweep): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on); uptime_s 1011.

### Lamp (same scene plus a lamp)

| profile | set | valid reps | fps | longest gap ms | gap p95 ms | frame KiB | link Mbps | brightness |
|---|---|---|---|---|---|---|---|---|
| xga q24 @18 | auto-default | 2 / 2 | 11.23 | 147.7 | 109.9 | 25.61 | 2.39 | 105.1 |
| xga q24 @18 | auto-gain32x | 2 / 2 | 11.29 | 135.1 | 115.7 | 25.81 | 2.42 | 108.6 |
| xga q24 @18 | auto-gain128x | 2 / 2 | 11.21 | 168.1 | 109.4 | 26.45 | 2.47 | 104.7 |
| xga q24 @18 | manual-e300-g0 | 2 / 2 | 11.21 | 154.1 | 106.2 | 25.90 | 2.42 | 100.2 |
| xga q24 @18 | manual-e600-g10 | 2 / 2 | 11.29 | 162.1 | 116.1 | 18.05 | 1.69 | 248.7 |
| xga q24 @18 | manual-e1000-g20 | 2 / 2 | 11.21 | 156.0 | 113.0 | 15.66 | 1.47 | 254.0 |

INFO (before sweep): cpu_mhz 160; flash_mode_cfg "not-qio", flash_freq 40m,
runtime.flash_io dio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on); uptime_s 1434.

Baseline firmware (NCB), XGA q24, 10 s x 2 per cell, scene with a bright window,
restored defaults. Every set reached 10.7 to 11.3 fps in both lighting
conditions. Manual 600/10 and 1000/20 were overexposed (brightness 242 to 254)
and gave smaller frames (15.7 to 19.8 KiB against 25.6 to 27.2 KiB). The lamp
changed brightness by at most 7 points. The low fps of the 2026-10-11
wall-corner run was not reproduced in this scene; no dim condition was tested.

## XCLK sweep, detailed scene, NC (2026-10-13)

Source: `benchmarks/results/phase2-cam-xclk-detail-20261013-211343.json`
(`--label xclk-detail --expect new`), produced by `benchmarks/phase2_cam.py`
on 2026-10-13. Bench firmware NC (240 MHz, tuning block): cpu 240,
flash qio/80m/qio; fixed manual exposure 300/0; dark detailed scene; 20 s runs,
2 repetitions per profile. `real size` is the dominant frame size read from the
JPEG headers.

| profile | valid reps | fps PC | all reps | fps device | frame KiB p50 / p95 | frame interval p95 ms | link Mbps | stalled s | slow_sends | min free internal | real size | end-marker ok % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| uxga q16 @18 | 2/2 | 10.44 | [9.65, 11.24] | 10.66 | 64.7 / 76.8 | 162.6 | 5.78 | 2 | 119 | 46931 | 1600x1200 | 100.0 |
| uxga q16 @20 | 1/2 | 12.53 | [12.53] | 12.55 | 59.0 / 60.8 | 106.8 | 6.05 | 0 | 8 | 46871 | 1600x1200 | 100.0 |
| uxga q16 @22 | 2/2 | 5.97 | [5.88, 6.06] | 5.95 | 228.1 / 231.9 | 278.2 | 11.13 | 0 | 240 | 43663 | 1600x1200 | 100.0 |
| uxga q16 @24 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |
| xga q12 @18 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |
| xga q12 @20 | 2/2 | 12.47 | [12.47, 12.47] | 12.64 | 29.8 / 29.9 | 88.5 | 3.08 | 0 | 1 | 59959 | 1024x768 | 100.0 |
| xga q12 @22 | 2/2 | 13.79 | [13.76, 13.82] | 13.87 | 59.1 / 60.4 | 84.6 | 6.72 | 0 | 2 | 45775 | 1024x768 | 100.0 |
| xga q12 @24 | 0/2 | - | [] | - | - / - | - | - | 0 | 0 | None | None | - |

INFO (before → after): cpu_mhz 240; flash_mode_cfg "qio", flash_freq 80m,
runtime.flash_io qio; flash_id 0x5e4016; radio protocol_bitmap 7, bandwidth HT40,
channel 1, max_tx_power_q 78; reset_reason 1 (power-on) → panic;
uptime_s 758 → 378.

Failed attempts:

- uxga q16 @20 rep 2 try 1: no data received (recv timeout (device stopped sending))
- uxga q16 @24 rep 1 try 1: no data received (recv timeout (device stopped sending))
- uxga q16 @24 rep 2 try 1: cannot read config back
- xga q12 @18 rep 1 try 1: config POST failed (HTTP 0, device unreachable)
- xga q12 @18 rep 2 try 1: config POST failed (HTTP 0, device unreachable)
- xga q12 @24 rep 1 try 1: no data received (recv timeout (device stopped sending))
- xga q12 @24 rep 2 try 1: no data received (recv timeout (device stopped sending))

Bench firmware NC (240 MHz, tuning block), fixed manual exposure 300/0 (dark
scene), 20 s runs. fps followed 11.25 x XCLK/18 (12.47 and 12.53 at 20 MHz).
At 22 MHz XGA and UXGA frames were 2x to 3.5x larger and the saved pictures
were noise or did not decode; the runner then marked them valid, which has
been fixed. At 24 MHz no frames arrived and the device later reported reset
reason panic (uptime 378 s at the end of the run against 758 s at the start).
The device was unreachable for the XGA 18 MHz step.

