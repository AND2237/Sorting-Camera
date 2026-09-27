# ADR-0010: Operating profiles, per-profile frame-rate floors, and the HD ceiling

_Status: accepted 2026-09-27_

## Context

§34 asks for "highest practical image quality / sustained minimum 15 FPS /
preferred 20 FPS+", with the exact combination established empirically. Phase 5
was supposed to find out what raises the frame rate. It found the opposite: the
frame rate is not ours to raise at 1280×720.

Measured with the app in bench mode (120 s after a 10 s warm-up, device and PC
metrics over the identical window):

- App frame rate equals device frame rate within 0.3% in every run, with zero
  dropped, stale or overwritten frames, at 19–36% of one core and 2–23 ms p95
  end-to-end PC age. The PC pipeline is not the constraint.
- Peak link throughput is 4.36 Mbps (545 KB/s) on a 802.11n 40 MHz softAP link
  with one station — a fraction of what the link carries. The link is not the
  constraint.
- **fps is the camera's frame-production rate.** 1280×720 tops out near 11.2 fps,
  confirmed independently by the Phase 4 harness (HD q24 11.25, q36 11.18 fps).
  Reaching 15 fps at HD needs +34%, and the only measured lever is XCLK, which
  is already at its safe limit: 20 MHz is worse than 18, 22 MHz degrades to
  2.2 fps with 246 KB frames, and 24 MHz and above produce no frames at all.

The project owner had already chosen 1280×720 as the production resolution
(2026-09-27), preferring image quality first. The gate and that choice cannot
both hold as written, so the conflict had to be resolved rather than papered
over.

## Decision

1. **1280×720 (HD) q12, XCLK 18 MHz, fb3, `CAMERA_GRAB_LATEST`, PSRAM remains the
   production default.**
2. **The frame-rate floor is scoped per profile instead of globally.** HD carries
   a floor of ≥7 fps (provisional, below the worst of five runs at 7.74–11.17),
   and the ≥15 fps target applies to the profiles that can meet it: SVGA ≥20
   (measured 22.42), VGA ≥20 (22.51), QVGA ≥40 (44.94). HD's floor will be
   tightened to the 1 h soak's sustained figure. A 4.9 min partial soak measured
   8.08 fps at 58.5 KB frames with zero failures, recoveries or reboots and a
   −13.8 KB heap drift inside a 17 KB noise band; it does not certify
   long-duration stability, so the ≥1 h soak stays open.
3. **The measured numbers are published as they are**, including the fact that
   the §34 acceptance target is not met at the production default and the
   hardware reason why.
4. **Per-pixel quality is the unit of comparison.** HD q24 and SVGA q36 both
   encode 0.033 bytes per pixel — equal per-pixel quality — and SVGA delivers
   33.6 fps against HD's 11.2. HD q12 is 0.054 B/px, 1.6× the per-pixel quality,
   at the same frame rate. The trade is 3× the frame rate or 2.25× the pixels.
5. **Camera task affinity stays at the ESP-IDF default (core0).** core1 measured
   −22.5% and no-affinity −16.7% at HD/q12 with frame sizes agreeing within
   1.2% across runs; both were reverted and reflashed. no-affinity is the only
   variant that wins a metric (latency, 12 vs 14 ms p50) but it costs 17% of the
   frame rate, and frame rate outranks latency.
6. **Wi-Fi/LwIP knobs are not pursued.** The link is not the constraint, and
   ESP-IDF's httpd already sets `TCP_NODELAY` around every chunk send
   (`httpd_txrx.c:494`) and disables it afterwards, so the Nagle lever is taken.
   There is no frame-rate headroom for them to win; re-open only if the link
   becomes the constraint.
7. **The firmware enforces the measured XCLK ceiling per resolution** so that
   HD plus a high clock cannot brick the camera (ADR-0009 recovery plus this
   validation).

## Consequences

- A viewer defaulting to HD will show roughly 8–11 fps. That is a property of
  this sensor and driver at 1280×720, measured three independent ways, not a
  tuning gap.
- The gate is met with margin from 800×600 downward, so a future adaptive
  profile (§12, Phase 6) can satisfy both the quality preference and the
  frame-rate target by selecting the profile per scene — that work is deferred,
  not abandoned.
- HD frame rate moves with scene complexity (7.7–11.2 fps tracking 58 KB → 45 KB
  frames), so any HD floor must tolerate that band; the soak will replace the
  provisional ≥7 with a sustained figure.
- Per-profile clocks mean the app must send XCLK together with the profile
  instead of once per session. The firmware already rejects unsafe combinations,
  so a stale clock is refused rather than fatal.
- Not claimed anywhere: 30 fps, very low latency, zero-copy or hardware
  acceleration, per §34.

## Evidence

- `benchmarks/results/phase5-baseline-20260927-*.json` (four profile baselines)
- `benchmarks/results/phase5-fb2-20260927-*.json` (fb2 vs fb3, four points)
- `benchmarks/results/phase5-xclk-20260927-*.json` (twelve-point XCLK sweep)
- `benchmarks/results/phase5-hddef-20260927-*.json` (HD q24 vs q12)
- `benchmarks/results/phase5-affinity-20260927-*.json` (three affinity variants)
- `docs/benchmark-results.md` — Step 0 baseline, the camera-is-the-ceiling
  finding, the A/B and sweeps, and the gate arithmetic
