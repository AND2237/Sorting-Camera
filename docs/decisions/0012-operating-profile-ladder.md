# ADR-0012: Operating profiles are measured data, and only HD has a lowered floor

_Status: accepted 2026-09-28_

## Context

§12 asks for a profile ladder — Maximum Quality, High Quality, Balanced, High
FPS, Low Latency, Custom — and requires that the automatic choice optimise by
the project's own priority, `image quality > FPS > latency`. It also says the
useful operating points must be found experimentally and that the example
resolution list is neither exhaustive nor necessarily optimal.

The tension is that §34 asks for image quality first *and* a ≥15 fps floor, and
ADR-0010 already established that 1280×720 cannot do both on this hardware: the
frame rate is the camera's frame-production rate, topping out near 11.2 fps, and
the only measured lever (XCLK) is already at its safe limit. The owner resolved
that by choosing HD as the production default and by scoping the floor per
profile rather than globally.

That left the profile ladder itself unbuilt, and a specific risk: a ladder is
the easiest place in this project to write plausible numbers. Nothing stops
someone declaring that SVGA q24 is "about 23 fps" when only two measurements
exist for it, or borrowing a neighbouring point's figure to fill a column.

## Decision

1. **Every figure in the ladder is a measurement recorded in
   `docs/benchmark-results.md`, and each entry carries the run it came from.**
   A profile with no citation is not a profile; it is a guess with a UI.
2. **Where two measurements of the same point exist, both are kept.**
   `measuredFps` is the Phase 4 envelope (fb3 / latest / psram / xclk 18 MHz,
   55 s per cell). `altMeasuredFps` is the Phase 4 130 s confirm run where one
   exists — and, for HD q12, which the confirm ladder never reached, the Phase 6
   D2 app-path baseline at the same resolution, quality and fb count
   (`phase6-baseline-20260928-hd-q12-x18.json`: one 120 s run measured three
   ways, mean 9.97 fps). They disagree by up to 25 percent on the same point,
   and that gap *is* the uncertainty.
3. **`altMeasuredFps == 0` means "no second reading was admitted", not "no
   second reading exists", and the evidence says which of the two applies.**
   Admission is literal: the reading must come from the same resolution, quality
   and fb count, because borrowing a neighbouring point is the fabrication this
   rule exists to prevent. Balanced (svga/q24) is the worked case — the confirm
   run *did* test it at 14.37 fps / 23,077 B, and `ADR-0008` decision 3 records
   it under "excluded by measurement" as below the floor, so the reading is
   cited as evidence but not admitted as `altMeasuredFps`. What no run ever
   measured was hd/q12, which is why HD's second reading comes from the D2
   baseline of decision 2 rather than from vga/q24's 18.97 fps. vga/q24's
   figures — 18.97 fps and 14,680 B — belong to 640×480, and carrying them on
   the svga/q24 profile was the defect this decision is written against.
4. **Automatic resolves to the largest pixel count clearing its own floor,
   computed from the table rather than hard-coded to a profile id.** The rule
   and the data stay separable: if a better HD measurement appeared, the same
   rule would pick it. On the measured envelope this yields **HD q12**, which is
   the production default ADR-0010 chose. That is the outcome, not the input.
5. **Only HD carries a floor below 15 fps, because only HD has a recorded
   decision to that effect.** §34's general floor is 15. ADR-0010 scoped HD to
   ≥7 provisionally. Maximum Quality (UXGA q4) is held to 15 and therefore
   *fails* it, at a measured 1.04 fps — and is labelled as failing in the UI
   rather than quietly given a floor of 1 so that it would pass. The project
   owner chose quality over frame rate at 0.92 MP, not at any cost.
6. **The engine learns the operating point from `/api/v1/status`, not from what
   was requested.** A profile is a claim about a point, and only the camera
   knows where it landed after clamping or rejecting a request. Verified against
   real hardware: HD q12 reports `High Quality`; the VGA q12 the camera was
   left in by an earlier bench run correctly reports `Custom`, because VGA q12
   is genuinely not a ladder entry.
7. **A shortfall must persist for three consecutive one-second windows, with 10
   percent of slack, before advice appears.** `docs/benchmark-results.md`
   records the same operating point measuring 9.95, 15.91 and 19.65 fps in one
   session as the scene changed. Reacting to a single slow window would be
   reacting to daylight.
8. **Manual mode never advises.** The user chose the point; the live frame rate
   is on screen regardless, and second-guessing a deliberate choice on every
   status refresh is noise.
9. **Applying a profile mid-stream is offered, not performed.** The firmware
   answers 409 to a `/config` change while a stream client is attached
   (ADR-0009). The button stops the stream, waits for the client to drop, sends
   the configuration, and reconnects — and says so, because silently dropping a
   live stream is a worse surprise than the slower frame rate the user just
   chose to keep.

## Consequences

- The application can satisfy the quality preference and, below 800×600, the
  frame-rate target — which is the resolution of the §34 conflict ADR-0010
  recorded as deferred rather than abandoned.
- A user on Maximum Quality will see roughly 1 fps. That is a measured
  property of 1.92 MP on this sensor, shown honestly, not a defect to be hidden
  by relabelling the profile.
- Per-point figures carry scene dependence, visible in the gap between the
  envelope and the confirm runs. The ladder's *ordering* held across both; the
  absolute values did not, and the floors are set to tolerate that.
- The ladder is not exhaustive. XGA and SXGA were measured and are omitted
  because both plateau near 11.2 fps, below the general floor, and neither
  improves on a neighbouring entry. `closestByResolution()` exists so advisory
  text can name the nearest measured point rather than interpolating.
- Changing the numbers in `ProfileEngine::buildLadder()` is a benchmark claim
  and needs a run behind it, not an edit.

## Evidence

- `docs/benchmark-results.md` — Phase 4 envelope table (all seven resolutions ×
  four qualities), the confirm ladder, the same-point drift note, the QVGA@q4
  frame-buffer incident, the per-resolution quality floors
- `benchmarks/results/phase6-baseline-20260928-hd-q12-x18.json` — HD q12's
  second reading: one 120 s app-path run measured three ways over the identical
  window (capture 10.00, delivery 9.97, decoded 9.96 fps; mean 9.97), with
  28,956 B mean and 28,033 B p50 frame size over 1,195 frames
- `docs/decisions/0010-operating-profiles-and-fps-floors.md` — the HD floor and
  the per-profile scoping
- `tests/tst_profileengine.cpp` — ordering, floors, evidence, matching, the
  three-window rule, and the assertion that a step down is always the adjacent
  rung. It also resolves every citation in `ProfileEngine::buildLadder()` back
  against `benchmarks/results/` at run time (`SCAM_BENCH_DIR`), so a figure
  that is not in the artifact it names, or that was measured at a different
  resolution or quality, fails the build's test suite instead of reaching a UI
