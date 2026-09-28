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
   55 s per cell). `altMeasuredFps` is the Phase 5 130 s confirm run where one
   exists. They disagree by up to 25 percent on the same point, and that gap
   *is* the uncertainty.
3. **Where the confirm ladder did not test a point, `altMeasuredFps` is 0 and
   the evidence says so.** The confirm ladder covered svga/q36, vga/q24, vga/q36
   and qvga/q12 / q24 / q36 — it did not cover svga/q24. Borrowing vga/q24's
   18.97 fps for the svga/q24 profile would have been a fabricated data point,
   and it was nearly committed.
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
- `benchmarks/results/phase6-baseline-20260928-hd-q12-x18.json` — HD q12 at
  10.00 fps, the only ladder point with a three-run measurement
- `docs/decisions/0010-operating-profiles-and-fps-floors.md` — the HD floor and
  the per-profile scoping
- `tests/tst_profileengine.cpp` — ordering, floors, evidence, matching, the
  three-window rule, and the assertion that a step down is always the adjacent
  rung
