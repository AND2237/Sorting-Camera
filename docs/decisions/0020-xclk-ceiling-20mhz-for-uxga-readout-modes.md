# ADR-0020: Sensor clock ceiling 20 MHz for HD, XGA, SXGA and UXGA

_Status: ceiling applied 2026-10-14 (firmware built and linked, not yet flashed or soaked);
20 MHz itself is not yet shown to be stable._

## Context

`camera_xclk_max_mhz()` limited only HD (20 MHz); every other size accepted up to 27 MHz, and
the control API accepted 6 to 27 MHz. The frame-rate plateaus recorded in this repository
(11.25 fps for the modes read out from the 1600x1200 sensor window, 22.5 fps for SVGA and VGA)
scale with the sensor clock, so the clock was the lever to test. The camera-on run
`xclk-detail` (2026-10-13, bench firmware NC: 240 MHz, tuning block, fixed manual exposure 300 /
gain 0, a dark detailed scene, 20 s runs) gave:

| Profile | XCLK | fps (PC) | frame KiB p50 | link Mbps | What the picture was |
|---|---|---|---|---|---|
| UXGA q16 | 18 | 10.44 (9.65, 11.24) | 64.7 | 5.78 | not inspected |
| UXGA q16 | 20 | 12.53 (1 of 2 valid reps; the other stopped sending) | 59.0 | 6.05 | not inspected |
| UXGA q16 | 22 | 5.97 | **228.1** | 11.13 | sample JPEG does not decode: corrupt data |
| UXGA q16 | 24 | no frames; config then unreadable | - | - | device later reported reset reason **panic** |
| XGA q12 | 18 | not measured (device unreachable after the UXGA 24 MHz step) | - | - | - |
| XGA q12 | 20 | 12.47 (12.47, 12.47) | 29.8 | 3.08 | not inspected |
| XGA q12 | 22 | 13.79 | **59.1** | 6.72 | sample is pure noise |
| XGA q12 | 24 | no frames | - | - | - |

The device was reachable again for the XGA 20 MHz step; the final INFO showed uptime 378 s
against 758 s at the start of the matrix, i.e. a reboot during the run, which lines up in time
with the UXGA 24 MHz step. The cause of the panic is not known (no serial log).

The runner marked the 22 MHz runs VALID: size, end marker and device counters were all fine
while the picture was garbage. The runner now also decodes the sample picture, measures pixel
noise, compares frame size with the same profile at a lower clock, and refuses profiles above
`--max-xclk` (default 20), so this cannot recur unnoticed.

## What the data supports

- fps scales with the clock as `11.25 x XCLK / 18`: predicted 12.50 at 20 MHz (measured 12.47 and
  12.53) and 13.75 at 22 MHz (measured 13.79, but with corrupt pictures).
- UXGA q16 frames of 59 to 65 KiB were carried at 10.4 to 12.5 fps (5.8 to 6.1 Mbps) with the
  tuning block, so no byte-rate limit appeared at that frame size on this scene. The earlier
  "about 2.2 ms per KiB" relation (docs/benchmark-results.md) was not reproduced here.
- 22 MHz is not usable for XGA or UXGA on this board, and 24 MHz stopped the stream and was
  followed by a panic.

## Decision

1. `camera_xclk_max_mhz()` returns 20 for HD, XGA, SXGA and UXGA and stays 27 for the other sizes
   (SVGA was measured clean up to 27 MHz). A stored configuration above the ceiling is already
   discarded at boot (camera.c, load of the saved record), so a saved 24 MHz setting can no
   longer cause a restart loop.
2. The default clock stays 18 MHz in every profile until a 20 MHz soak passes.
3. SXGA was not measured; it shares the sensor readout with the other three and gets the same ceiling.

## Consequences

- At 20 MHz the modes read out from the full sensor window are capped near 12.5 fps (sensor
  readout), and SVGA/VGA near 25 fps if the same scaling holds (not measured at 20 MHz here).
  15 fps at 1600x1200 would need about 24 MHz, which this board did not run.
- One UXGA 20 MHz repetition stopped sending. Whether 20 MHz is stable needs longer runs, with
  a picture saved at each clock (the runner now names samples by clock) before the default moves.
- The Qt application requests 18 MHz in every profile, so it is unaffected.

## Related

- ADR-0018, ADR-0019; `docs/benchmark-results.md` (XCLK sweeps); `benchmarks/phase2_cam.py`
