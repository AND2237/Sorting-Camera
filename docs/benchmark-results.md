# Benchmark results

**No harness measurements yet.** Harness-run numbers (Phase 3+) are filled only by actual runs of `benchmarks/` harnesses. Fabricated or estimated numbers are forbidden.

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

## Run log template

| Run | Date | FW ver | App ver | Transport | Resolution | Quality | fb/grab/xclk | Router/PC | Duration | Result summary |
|-----|------|--------|---------|-----------|------------|---------|--------------|-----------|----------|----------------|
| — | — | — | — | — | — | — | — | — | — | — |

## Phase 3 — transport shootout

(Tables per transport: receive FPS, decode ms, CPU %, drops, corrupt, reconnects, p95 latency, notes.)

## Phase 4 — camera matrix

(Full resolution × quality × buffer grid; operating-point ladder result.)

## Phase 5 — optimization deltas

(Baseline vs each isolated change; link ADRs in `docs/decisions/`.)

## Soak tests

(≥1 h runs: memory trend, leak/fragmentation/stall findings.)
