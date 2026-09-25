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

## Phase 4 — camera matrix

(Full resolution × quality × buffer grid; operating-point ladder result.)

## Phase 5 — optimization deltas

(Baseline vs each isolated change; link ADRs in `docs/decisions/`.)

## Soak tests

(≥1 h runs: memory trend, leak/fragmentation/stall findings.)
