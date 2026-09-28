# ADR-0013: Per-camera device objects behind a registry, one camera in the view

_Status: accepted 2026-09-28_

## Context

§27 requires that the architecture "must not make multi-camera support
unnecessarily difficult", that abstractions such as Device / CameraDevice /
CameraConnection / StreamSession / Transport / Decoder / Recorder be used, and
specifically that the architecture **not** be built around a single global
camera singleton. It also permits the first version to show one stream.

The code before this decision satisfied the second half of that sentence and
not the first. There was exactly one `FrameBus`, one `MjpegClient`, one
`DeviceStatus`, one `Recorder`, one `SnapshotWriter`, one `SessionState` and
one `StreamStats` — all stack locals in `main()`, with `main.cpp` wiring each
signal by hand. Adding a second camera would have meant turning one of those
into a lookup-by-index at every use site, which is precisely the refactor §27
is trying to prevent while it is still cheap.

Discovery had already been built to hold many cameras, so the gap was not in
discovery. It was that everything downstream of it was single-camera by
construction.

## Decision

1. **`CameraDevice` owns one camera's entire pipeline** — control channel,
   stream, latest frame, metrics, recording, snapshots, session state, profile
   engine, notification centre — and knows nothing about any other camera.
2. **`DeviceRegistry` owns the set** and is the only place that knows how many
   cameras exist. It holds no transport, no decoder and no view.
3. **The view shows one camera, as §27 allows.** QML was not rewritten to chase
   a variable: `main.cpp` re-points the existing context property names at the
   newly active device, and the image provider resolves the active frame bus per
   request. Everything below the view is per camera; only the view is single.
4. **Devices are keyed by the camera's own device id, never by address.** This is
   the decision that does the most work. A camera that answers on a new address
   is the same camera; keying by address would strand its stored credential, its
   session state and its recording target on every address change.
5. **Acquiring is not selecting.** An announce for a second camera must not pull
   the view away from the one being watched.
6. **`releaseStale()` never collects the active device**, whatever its age.
   Dropping the camera the user is looking at because it stopped announcing
   would be a worse failure than keeping one stale object.
7. **`setActive()` on an unknown id leaves the active device untouched** and
   returns false. A bad id from the UI must not blank the view.
8. **The QML-facing entry points validate ports before anything reaches a
   socket**, because that is where values arrive untyped from an announce.
9. **The image provider takes a resolver, not a fixed bus.** The active camera
   can change between two frames, and a provider holding one bus would keep
   painting the previous camera's picture.

## Consequences

- A second camera is a second entry in the registry, not a second architecture.
  A second simultaneous *view* is still future work and is not claimed here: the
  view is single-camera by design, which §27 permits.
- Every per-device object carries a small cost — a 1 s timer per camera for the
  profile engine's shortfall sampling — which is negligible at the scale
  currently supported and is the price of not being single-camera.
- `main.cpp` grew rather than shrank, because it now owns the mapping from a
  camera to the QML names. That mapping is one `setContextProperty` block and
  is the only place that knows the QML vocabulary.
- The bench harness is unaffected and was re-measured after the move: VGA q12,
  22.1 fps over 133 frames with zero drops, matching the 22.51 fps in ADR-0010.
  A refactor that quietly halved the frame rate would not have been caught by
  the unit tests alone.
- The structural claims are asserted by `tests/tst_deviceregistry.cpp` rather
  than left to review: two devices with wholly separate pipelines, one id
  surviving an address change, an announce not stealing the view, an unknown id
  leaving the active device alone, and stale release sparing whatever is
  watched.

## Evidence

- `docs/architecture.md` — device and registry sections
- `desktop/src/CameraDevice.{h,cpp}`, `desktop/src/DeviceRegistry.{h,cpp}`
- `tests/tst_deviceregistry.cpp` — 10 structural assertions
- `benchmarks/results/phase6-baseline-20260928-hd-q12-x18.json` — unchanged
  baseline after the refactor
