# ADR-0014: Diagnostics capture unconverted log calls and count what they suppress

_Status: accepted 2026-09-28_

## Context

§38 requires structured logs at five levels (DEBUG, INFO, WARNING, ERROR,
CRITICAL), categorised per subsystem, covering discovery, authentication,
connection, transport, frame reception, frame loss, decoding, rendering and
recording. It adds two constraints that decide the design: **"Do not flood logs
at high FPS"** and **"Use rate-limited / statistical reporting where
appropriate."**

The code before this decision logged with `qDebug()`/`qInfo()` at whatever level
felt right, prefixed by hand-typed tags (`[stream]`, `[auth]`, `[config]`,
`[discovery]`). Two problems followed from that. First, a per-frame fact such
as frame size or pipeline age has no line-rate representation that is both
readable and free — at 20 fps a line per frame is 72,000 lines an hour and it
describes the very thing it is slowing down. Second, a rate limiter that
silently drops what it suppresses makes a flapping link look like a quiet one,
which is worse than not logging it at all.

§26 separately requires error and status notifications, and the conditions
worth notifying about were already being detected — the session state machine
names eight states, the recorder reports failures — but each was surfaced in a
different corner of the window, and the most serious ones had the least visible
treatment.

## Decision

1. **A process-wide `Diagnostics::Facility` installs a Qt message handler.**
   Plain `qDebug()`/`qInfo()` calls are captured with a category taken from
   whichever `Diagnostics::Scope` the caller is inside, at the level Qt already
   assigned. The alternative — rewriting every call site into a categorised
   helper — puts the burden on each call site and makes an unconverted one an
   invisible hole. One guard per subsystem cannot be forgotten in the same way.
2. **One level vocabulary shared with `NotificationCenter`.** A condition cannot
   be a warning in the log and an error on screen because two enums drifted
   apart.
3. **Each level has a per-window budget; CRITICAL is unlimited.** A critical
   that fires once matters, and one that fires fifty times matters more.
4. **Suppression is counted, never hidden, and the count rides on the next line
   that gets through.** A log that swallowed 97 retries would answer "how often
   does this fail" with a confident wrong number.
5. **`setRateLimit()` resets the spending window but preserves the pending
   suppression count.** Raising a level at runtime must not erase the record of
   the burst it was raised to see.
6. **The high-rate quantities are counters with min/max, not log lines.** A last
   value of 31000 bytes says nothing about whether a stream is steady; the
   spread does. Frames, drops, received count, frame size and frame age all go
   this way.
7. **Drops are counted as the delta since the previous sample.** Counting the
   running total would report a growing number whether or not anything was
   dropped.
8. **Information expires; warning and above waits for a dismissal.** A user does
   not need to be told twice that the camera was found, and nobody has
   acknowledged an error until they act on it.
9. **`postOnce()` collapses repeats of one keyed condition**, so a flapping link
   cannot bury the rest of the list. Deduplication is per key, not a single
   global slot, so one repeating fault cannot hide another.
10. **The diagnostics view is on demand (F12), not always visible.** A permanent
    log window costs space the picture needs and hides the thing it explains.

## Consequences

- Log volume is bounded by construction rather than by discipline, and a
  truncated log says how much it truncated.
- A subsystem that logs without a scope lands in `other` rather than nowhere, so
  an unannotated call site is visible as unattributed rather than invisible.
- The facility's message handler also captures the test framework's own output.
  That is correct behaviour, and it cost a test its original assumption that the
  buffer contains only what it logged; the test now scopes its assertions to
  rows it created.
- The counter spread is available for free, which is what would be needed for
  p95 reporting in Phase 7 without re-instrumenting the frame path.
- Notifications are now one collection rather than several corners of the
  window, and the session state machine is the single source for the conditions
  that matter most.

## Evidence

- `docs/architecture.md` — diagnostics and notifications sections
- `desktop/src/Diagnostics.{h,cpp}`, `desktop/src/NotificationCenter.{h,cpp}`
- `tests/tst_diagnostics.cpp` — 13 checks: level/category vocabulary, rate
  limiting, suppression accounting across a limit change, counter spread, ring
  buffer bounds
- `tests/tst_notificationcenter.cpp` — 12 checks: ordering, expiry, sticky
  levels, per-key deduplication, dismissal, counting

### Two defects the tests caught

Both would have shipped silently, and both are recorded because the failure
mode is instructive:

- `QVariantList::append(QVariantList)` **flattens** the inner list instead of
  storing it as a row. `recentEntries()` returned twelve integers where it
  should have returned three rows, and the diagnostics panel would have
  rendered garbage with no QML error to explain it. Fixed by appending
  `QVariant(row)`.
- `post()` leaves the deduplication key empty, and the key lookup matched the
  first empty key, so consecutive unconditional posts collapsed onto one card
  instead of appearing as distinct notifications.
