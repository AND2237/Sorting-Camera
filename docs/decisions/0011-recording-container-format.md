# ADR-0011: Recording container format

_Status: accepted 2026-09-28_

## Context

§21 requires the recording format to be **researched and selected**, not
assumed, and scores the options on six criteria: exact preservation, storage
efficiency, easy retrieval, sequential write performance, metadata support, and
recovery after unexpected application termination. The primary archival mode
must keep the original JPEG payload recoverable byte-for-byte, so any format
that decodes or re-encodes is disqualified before the comparison starts.

Four candidate families were evaluated.

### 1. One JPEG file per frame

Exact preservation is trivial and each file is independently recoverable, so a
crash costs at most one file. Against that: at 10 fps a one-hour session creates
36,000 files, which degrades directory listing and grows the MFT; the per-file
metadata available (filename, timestamps) cannot carry device, firmware,
resolution or sequence; and per-frame write performance is the worst of the set
because every frame pays for an open/create/close.

### 2. MJPEG-like stream (concatenated JPEGs, no framing)

Zero overhead, one sequential append, and recovery is a marker scan. JPEG makes
this scan reliable: `FF D8` starts a frame and `FF D9` ends it, and `FF` bytes
inside entropy-coded data are always stuffed as `FF 00`, so `FF D9` cannot occur
inside a frame. The fatal weakness is metadata — nothing in-band records what
the file is, which device produced it, or when each frame arrived, so retrieval
and provenance depend entirely on external bookkeeping.

### 3. Container with an external index (chosen)

A short magic + JSON header, then each frame written as a little-endian `u32`
length followed by the untouched JPEG bytes, with a JSON sidecar holding
per-frame `offset/seq/ts/w/h`.

- **Exact preservation:** the payload is copied as received; no decode.
- **Storage efficiency:** 4 bytes per frame — 0.007% at HD's ~58 KB frames.
- **Sequential write performance:** a single append, one or two `write` calls
  per frame, `QIODevice::Unbuffered` so bytes reach the OS on every frame.
- **Metadata:** a header (device id, firmware, resolution, quality, start time)
  and a per-frame index.
- **Recovery:** the index is a *convenience*, not a dependency. `Recorder::scan`
  rebuilds the frame table from the container alone: read the length, verify it
  is plausible, verify the bytes are a well-formed JPEG, repeat — and stop at
  the first frame that fails, which is exactly where a truncated write ends.
  Everything before that point is intact. This is what makes a killed process
  cost only the frames since the last index flush (≤60), never the file.

### 4. Existing container formats (AVI/MKV/MP4)

AVI with an MJPEG stream would carry JPEG frames, but its index is written at
the *end* of the file, so a crash leaves a container with no index — the exact
failure mode this requirement is about. Matroska and MP4 need a muxer
dependency, and bringing FFmpeg in for this is a licence and build-cost
decision the project has not taken. Nothing in the container above is lost by
declining.

## Decision

Recording uses the **indexed length-prefixed container** (`.scamrec` + `.json`
sidecar), implemented in `desktop/src/Recorder.*`.

1. Frames are written as the exact bytes received — no decode, no re-encode,
   ever (§21).
2. The sidecar index is written at most once every 60 frames and again on a
   clean stop; it is never the only copy of the structure.
3. Snapshot (§22) writes the current frame's bytes straight to `.jpg` and
   verifies the on-disk size matches what was written.
4. `SnapshotWriter` pulls bytes from `FrameBus` rather than taking them as a
   QML argument, so frame payloads never cross into the scripting layer.

## Consequences

- A `.scamrec` file is not directly playable by third-party tools; retrieval
  goes through the index or `Recorder::scan`. That is accepted for an archival
  format whose stated priority is exactness.
- Frame width/height and timestamp exist only in the sidecar, because the
  container deliberately carries no per-frame metadata. After a crash the
  rebuilt index recovers `offset/seq/bytes` for the tail frames but not their
  dimensions or timestamps; those are available for every frame the sidecar
  reached.
- Adding metadata in-band later would change the format version, which the
  header already carries as `formatVersion`.
- Recording runs on the frame-delivery path, so it must never block there. It
  is a bounded append per frame with periodic index flushes; recording overhead
  still needs a measured baseline before it is claimed to be negligible.

## Evidence

- `tests/tst_recorder.cpp` — round-trip SHA-256 identity over 12 frames,
  header survival, sidecar content at 70 frames, and a truncation test that
  kills the recorder without `stop()` and proves the intact frames are still
  recovered byte-for-byte.
- `tests/tst_capture.cpp` — end-to-end: a stub MJPEG server feeds the real
  parser and worker thread; the snapshot and the recording both hash-equal to
  the bytes the "camera" sent.
- `docs/architecture.md` → "Recording & snapshot"
