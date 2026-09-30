# Testing

_Status: harnesses added incrementally from Phase 2 onward._

## Levels

1. **Host unit tests (desktop):** protocol header encode/decode, CRC, frame reassembly (incl. truncated/oversized/malformed input), latest-wins drop policy, metric math (FPS/bitrate/latency aggregation), config validation, recorder byte-identity (write → read → SHA-256 equal), discovery packet parse. Framework: Qt Test or Catch2 (pick one and stay with it; Qt Test preferred — no new dependency).
2. **Firmware host-buildable tests:** pure functions (header build, CRC, config apply validation) compiled on host where feasible; on-device smoke via Unity/`idf.py -T` if enabled.
3. **Integration (manual/semi-auto scripts):** spin app against firmware (or a PC-side MJPEG/TCP/UDP **fake camera** server under `tools/` for CI without hardware) → verify connect, auth, stream, snapshot, record, reconnect. `tools/config_contract_smoke.py` is the committed version of that: it starts `tools/fake_camera.py`, drives the ADR-0017 config contract (read-only GET, POST+JSON writes, 400/415 refusals) and reads the stream's raw bytes to prove the fake emits the chunked transfer the firmware sends. Needs Python and no Qt, no hardware.
4. **Soak (Phase 8):** ≥1 h continuous stream while toggling: resolution changes, snapshot, record start/stop, PC Wi-Fi leave/rejoin (AP disruption), client kill/reconnect, camera reboot. Watch: memory trend, drop counters, stale-frame age, UI responsiveness, thread safety (TSAN on host tests where possible).

   _Status: deferred to Phase 8 as a **final validation** activity, designed
   around the finished firmware + Qt application + networking + recording + UI
   architecture rather than a repeat of the Phase 5 streaming procedure.
   Telemetry needed for it is already in place and is not removed in the
   meantime. Until it runs, no long-duration stability is claimed — see
   `benchmark-results.md` → "Soak tests"._


## Host unit test suites (desktop)

All under `tests/`, Qt Test, built by `scam_add_test` and run with `ctest` from
`desktop/build`. Sixteen suites. The first fifteen were green in the last full
`scripts\ci.ps1` run (2026-09-30, `ctest` 15/15); two of those were added
during Phase 7 completion: `tst_capabilities` for the §28 machine probe, and
`tst_accessibility` for the §25 QML annotation sweep. The sixteenth,
`tst_appmetrics`, plus new tests inside `tst_devicestatus` and `tst_capture`
(Task B2's remainder — §33 metric arithmetic, §14 snapshot failure paths, and
the `authRequired` Bearer header), were written the same day and are recorded
in `audits/PHASE7_REMEDIATION.md` as **written, not yet run**: the owner's
test pass runs them first.

| Suite | Covers |
|---|---|
| `tst_authclient` | PBKDF2 proof vector against a reference, nonce/password dependence, missing-credential and live-device paths |
| `tst_configwritesequence` | the ordered config write chain: every requested setting is emitted in order, an unset option does not truncate what follows, an empty run writes nothing, `takeNext()` stops at the end |
| `tst_credentialstore` | DPAPI round-trip, overwrite, clear, move, ciphertext-not-plaintext, empty-key guard — **not** covered: wrong-password rejection and the storage-unavailable fallback, both genuinely untested code paths (DD-10) |
| `tst_discovery` | announce parsing, dedupe by device id not address, ageing, subnet broadcast targets, live-device test gated on `SCAM_DISCOVERY_PORT` |
| `tst_sessionstate` | all eight §24 states, branch ordering, severity mapping — pure, no event loop |
| `tst_userprefs` | defaults, full round-trip, write de-duplication, survival across instances; QSettings redirected to a scratch dir so it never touches the real credential file |
| `tst_devicestatus` | capabilities not requested before a status poll, one control per write, unknown control produces **no** request, 409 surfaces the camera's message, device switch drops and re-asks, reset applies supported defaults in one write, concurrent writes serialise, and — with `authRequired` on the stub — the first poll goes out **without** an `Authorization` header while every status/capabilities request after sign-in carries `Bearer <token>` |
| `tst_deviceregistry` | two devices with separate pipelines, one id surviving an address change, announce not stealing the view, unknown id harmless, stale release sparing the active device, port validation |
| `tst_profileengine` | ladder ordered by quality first, every entry cites a measurement, **floors only lowered by a recorded decision**, automatic follows the rule rather than a hard-coded id, three-window shortfall rule, manual mode never advises |
| `tst_diagnostics` | level/category vocabulary, rate limiting, suppression counted and preserved across a limit change, counter spread, ring-buffer bounds |
| `tst_notificationcenter` | ordering, info expiry vs sticky warnings, per-key dedup, dismissal, counting |
| `tst_recorder` | SHA-256 byte-identity of recorded frames, header survival, sidecar flush timing, truncated-tail recovery, double-start refusal, snapshot identity |
| `tst_capture` | in-process MJPEG stub driving the **real** parser and worker: raw bytes survive the stream on **both** wire formats (identity, and chunked split inside the `Content-Length` digits the way the firmware sends it), snapshot hashes equal the source, recording round-trip equal, the 5-step reconnect ladder with both watchdogs, a 7-case malformed-input matrix that asserts the *named* failure reason, and the snapshot failure paths (no frame yet, directory that cannot be created, the error clearing on the next successful save). Plus a **live-device** byte-identity test (below) |
| `tst_capabilities` | §28 machine probe: every bullet answered (CPU, memory, graphics backend and acceleration, decode path, display, interfaces, touch), every value an observation rather than a guess (build architecture, measured memory on Windows, the honest no-hardware-decode line), the flags agreeing with the list, and `refresh()` re-probing |
| `tst_accessibility` | §25 source tripwire over `Main.qml`: every `Button`/`Slider`/`ComboBox`/`TextField`/`CheckBox`/`ToolButton` carries an `Accessible` name **and** role inside its own block (a neighbour's annotation cannot stand in), every `Accessible.role` value exists in `QAccessible::Role`, the non-control surfaces are named, and the count of named surfaces cannot quietly shrink |
| `tst_appmetrics` | §33 metric arithmetic: percentiles against a hand-computable distribution (p50/p95/p99/min/max/mean), the 200 000-sample cap that drops no count, every counter and series, rate fields finite and non-negative, CPU/working-set sanity bounds, `writeJson` merging benchmark metadata without replacing metrics and failing on an unopenable path, monotonic `nowMs`/`nowUs` |

Two defects were caught by these suites that no QML error would have explained:
`QVariantList::append(QVariantList)` flattens the inner list, and a
deduplication lookup that treated "no key" as a key. Both are recorded in
`docs/decisions/0014-diagnostics-and-notifications.md`.

Run them all:

```powershell
.\scripts\env.ps1
cmake --build desktop\build
cd desktop\build; ctest --output-on-failure
```

## Live-device tests

Three suites talk to real hardware when an environment variable is set, and
skip with a message naming the variable otherwise, so a normal `ctest` run
stays hermetic:

| Variable | Suite | What it proves against the device |
|---|---|---|
| `SCAM_TEST_HOST` + `SCAM_TEST_PASSWORD` | `tst_authclient` | the real PBKDF2 verifier, login and token path |
| `SCAM_DISCOVERY_PORT` | `tst_discovery` | real UDP announce parsing and dedupe |
| `SCAM_TEST_HOST` (+ optional `SCAM_STREAM_PORT`, default 81) | `tst_capture` | byte-for-byte preservation of the bytes the client's own receive path delivered — no packet capture is involved, so the reference is what the parser handed the app, not a tap on the wire (DD-10) |

`liveCameraBytesAreStoredVerbatim` exists because the stub in `tst_capture`
cannot prove the rule AGENTS.md states outright — a snapshot and a recording
hold the exact JPEG that arrived and are never decode-then-re-encode. Against
the real camera it asserts that every received frame is a complete JPEG
(SOI…EOI), that the snapshot file is byte-identical to the frame the bus held,
that re-encoding that frame at quality 90 produces *different* bytes (so the
file on disk is the original, not a re-encode), and that every frame in a
recording is byte-identical to one the camera sent. The stream port is
deliberately unauthenticated, so this check needs no credentials and no GUI.

## Mandatory test cases (Master Prompt §33)

- Protocol parsing: valid, truncated, bad magic, wrong version, oversized length, CRC mismatch → frame counted as corrupt, never displayed.
- Reassembly: interleaved frames, fragment gaps (UDP), seq wraparound.
- Discovery: response parsing, duplicate/aging entries, DHCP-IP change keeps stable ID.
- Auth: login success/fail, lockout, token expiry, replay rejection, redaction in logs.
- Reconnect: bounded backoff, no storm, state transitions surfaced to UI.
- Recording: byte-for-byte JPEG identity for every recorded frame (hash check), crash-recovery scan of index.
- Config: out-of-range sensor values rejected by firmware AND desktop; unsupported controls reported, not faked.
- Metrics: FPS/latency calculations against known synthetic streams.

## Rules

- Never disable or hide a failing test to make CI green; root-cause first (§35).
- Tests live in `tests/` (host) and co-located `*_test` where firmware-specific.
- Every bug fix gets a regression test when practical.
