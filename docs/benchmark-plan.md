# Benchmark plan

_Status: Phase 5 executed 2026-09-27 (see "Phase 5 — optimization protocol"
below and `docs/decisions/0010-operating-profiles-and-fps-floors.md`). Phases 0–4
have published results in `benchmark-results.md`._

## Ground rules

- Never publish numbers that were not measured on this project's hardware/software.
- Same scene, lighting, distance (1–3 m), AP channel, and link mode (softAP direct link is the ADR-0006 baseline) for A/B comparisons.
- Record for every run: date/time, firmware version, app version, transport, resolution, JPEG quality, fb_count, grab mode, XCLK, Wi-Fi settings (channel/band/RSSI), PC hardware, OS, Qt version, duration.
- Minimum run duration: 120 s steady-state after 10 s warm-up (soak runs separate, ≥1 h).
- Report median + p95, not just averages; include drop/corruption counters.

## ESP32 metrics

- camera init time; per-frame capture duration; JPEG frame size (min/median/mean/p95/max); capture FPS; heap (internal) and PSRAM free; task watermarks where available; RSSI; reconnect count; tx throughput (bytes/s) and tx FPS.

## PC metrics

- receive FPS, receive bytes/s; decode time per frame (median/p95); render FPS (scene-graph); dropped frames; stale-frame age; end-to-end software latency (device timestamp → render submit); CPU %, commit/memory, GPU % if measurable; discovery time; reconnect time; app startup time.

## Latency decomposition (req. §14)

| Segment | How measured |
|---|---|
| capture → JPEG complete | device-side timers around `esp_camera_fb_get` |
| device → network out | device-side, includes queueing |
| network transit | device tx timestamp vs PC receive timestamp (NTP-less: report as estimate, or same-LAN RTT-calibrated) |
| receive → decode complete | PC-side stopwatch |
| decode → render submit | PC-side stopwatch |
| **physical scene→display** | only if explicitly measured with an external clock/photodiode rig — otherwise state "not measured" |

## Phase 3 — transport shootout

For each transport (HTTP MJPEG, TCP framed, UDP packetized), identical camera config (start: VGA + mid quality, then 720p + best quality that holds ≥15 fps):

1. 120 s run, 1 client; record all ESP32 + PC metrics.
2. Stability pass: inject AP disruption (PC leaves/rejoins the camera Wi-Fi) / camera reboot / client kill-while-connected → verify bounded reconnect, no corrupt-frame display, counters honest.
3. Multi-client probe (2 clients) — note scaling behavior, not a ship gate for v1.
4. Fill `benchmark-results.md` with raw tables; write ADR for the winner with the stability veto applied.

## Phase 4 — camera configuration matrix

Dimensions: resolution (UXGA, 1280×720, 1024×768, 800×600, 640×480, …) × quality (driver 0–63 mapped to Highest/High/Balanced/Performance) × fb_count (1/2/3) × grab mode (latest vs continuation) × XCLK (20 MHz baseline; only deviate with measured reason) × Wi-Fi/LwIP knobs.

Deliverable: ordered operating-point list (best quality first) that sustains ≥15 fps (prefer ≥20) — the adaptive-profile ladder in the app is generated from this data, not guessed.

## Phase 5 — optimization protocol

One change at a time vs the frozen baseline build; every ADR carries: problem / baseline / change / measured result / tradeoffs / decision. Revert anything that does not win on a real metric or breaks stability.

_Outcome 2026-09-27:_ the PC pipeline was measured first and found **not** to be
the frame-rate constraint (app fps equals device fps within 0.3%, zero drops,
19–36% of one core), which redirected the work to the device. Executed: fb2 vs
fb3 at four points (fb2 loses everywhere), a twelve-point XCLK sweep at SVGA and
HD (XCLK is non-monotonic and resolution-dependent; 27 MHz is still rising at
SVGA while HD breaks at 24 MHz and above), a three-variant camera task affinity
A/B (core0 kept, core1 and no-affinity reverted), and the gate arithmetic that
established the 1280×720 ceiling near 11.2 fps. Wi-Fi/LwIP knobs were dropped
with the reason recorded rather than measured. Remaining: the 1 h soak, blocked
on the hardware power path.

## Acceptance (req. ≥34)

Sustained ≥15 fps (prefer ≥20) at the highest quality point proven by Phase 4; no unbounded latency growth; responsive UI; stable ≥1 h soak. Claims like "30 fps / zero-copy / hardware accelerated" forbidden unless demonstrated here.
