# ADR-0008: Camera operating ladder (Phase 4 configuration matrix)

- **Status:** Accepted (2026-09-26, Phase 4 harness results)
- **Context:** `docs/benchmark-plan.md` Phase 4 requires an ordered, measured
  operating-point list (best quality first) that sustains ≥15 fps (prefer ≥20) —
  the app's adaptive-profile ladder must be generated from this data, not guessed.
  Measured with `benchmarks/phase4_matrix.py` (envelope 7×4 cells @ 55 s, fb/grab
  cross 12 cells @ 55 s, confirm 11 points @ 130 s), XCLK 18 MHz, fbloc baseline
  psram, direct softAP link. Full numbers: `docs/benchmark-results.md` § Phase 4;
  raw JSONL: `benchmarks/results/phase4-20260925-fixed.jsonl`.
- **Decision:**
  1. **Standard tuning base: fb2 / latest / psram / XCLK 18 MHz.** At the boundary
     point (svga/q12) fb2/latest measured 15.91 fps vs fb3 11.44 and fb1 7.32;
     cont-grab is consistently slower than latest. All ladder points below were
     confirmed with this base.
  2. **The accepted ladder (quality-first, each point measured ≥15 fps for 120 s):**

     | # | Config | FPS mean | ≥20 |
     |---|---|---|---|
     | 1 | svga/q36 | 16.34 | no |
     | 2 | vga/q24 | 18.97 | no |
     | 3 | vga/q36 | 19.21 | no |
     | 4 | qvga/q12 | 42.00 | yes |
     | 5 | qvga/q24 | 44.61 | yes |
     | 6 | qvga/q36 | 44.93 | yes |

  3. **Excluded by measurement:** UXGA fails the floor at every quality (9.81 max
     with fb3 in envelope; 2.16 confirm with fb2); SXGA/HD/XGA plateau at ~11.2 fps
     (byte-rate cap) regardless of quality; svga/q12 (9.95 confirm), svga/q24
     (14.37), vga/q12 (14.87) fall below the floor; **QVGA@q4 is unusable** —
     its frames sit at the 15360 B (`w*h/5`) fb cap on detailed scenes → FB-OVF
     storm → stall-watchdog reboot (recorded `dead=True`).
  4. **DRAM fbloc is limited to ≤QVGA** (96000 B/buffer at SVGA exceeds the largest
     internal RAM block); the firmware rejects infeasible DRAM requests up-front
     with `ESP_ERR_NO_MEM` before touching the camera.
  5. **Boot default profile stays HD/q12/fb3** (locked decision) — adopting the
     ladder inside the app (adaptive profile selection) is a separate, explicit
     change; this ADR only fixes the measured menu it must choose from.

## Measured basis (2026-09-26, one confirm run per point)

- Envelope (fb3 baseline): SVGA q12/q24/q36 = 19.65/22.50/22.51; VGA q12/q24/q36 =
  22.49/22.50/22.03; QVGA q4–q36 = 24.20–45.01; UXGA all < 10; SXGA/HD/XGA all < 15.
- Absolute values drift with scene detail (same point svga/q12/fb2 measured
  19.65 → 15.91 → 9.95 across a daylight session as p50 frame size grew
  28089 → 36379 B) — ladder ordering is robust, per-point FPS carries scene
  uncertainty; re-validate per soak/environment changes.
- Zero camera wedges after the Phase 4 recovery hardening (PWDN power-cycle +
  SCCB bus recovery + config-endpoint self-repair); 14 brownout (`BOD`) reboots in
  run 4 were a USB power-supply issue, not firmware — check power before Phase 5.

## Consequences

- Phase 5 optimizes and soaks against ladder points, not against the full grid.
- QVGA@q4 (and any resolution whose q4 frames approach `w*h/5`) needs a
  frame-capacity change in the esp32-camera component before it can be considered.
- The Qt app can later expose the ladder as preset operating points / adaptive
  selection; that UI change should reference this ADR.
