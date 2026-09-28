# Testing

_Status: harnesses added incrementally from Phase 2 onward._

## Levels

1. **Host unit tests (desktop):** protocol header encode/decode, CRC, frame reassembly (incl. truncated/oversized/malformed input), latest-wins drop policy, metric math (FPS/bitrate/latency aggregation), config validation, recorder byte-identity (write → read → SHA-256 equal), discovery packet parse. Framework: Qt Test or Catch2 (pick one and stay with it; Qt Test preferred — no new dependency).
2. **Firmware host-buildable tests:** pure functions (header build, CRC, config apply validation) compiled on host where feasible; on-device smoke via Unity/`idf.py -T` if enabled.
3. **Integration (manual/semi-auto scripts):** spin app against firmware (or a PC-side MJPEG/TCP/UDP **fake camera** server under `tools/` for CI without hardware) → verify connect, auth, stream, snapshot, record, reconnect.
4. **Soak (Phase 8):** ≥1 h continuous stream while toggling: resolution changes, snapshot, record start/stop, PC Wi-Fi leave/rejoin (AP disruption), client kill/reconnect, camera reboot. Watch: memory trend, drop counters, stale-frame age, UI responsiveness, thread safety (TSAN on host tests where possible).

   _Status: deferred to Phase 8 as a **final validation** activity, designed
   around the finished firmware + Qt application + networking + recording + UI
   architecture rather than a repeat of the Phase 5 streaming procedure.
   Telemetry needed for it is already in place and is not removed in the
   meantime. Until it runs, no long-duration stability is claimed — see
   `benchmark-results.md` → "Soak tests"._


## Host unit test suites (desktop)

All under `tests/`, Qt Test, built by `scam_add_test` and run with `ctest` from
`desktop/build`. Twelve suites, all green as of the 0.2.0 release.

| Suite | Covers |
|---|---|
| `tst_authclient` | PBKDF2 proof vector against a reference, nonce/password dependence, missing-credential and live-device paths |
| `tst_credentialstore` | DPAPI round-trip, wrong-password rejection, storage-unavailable fallback |
| `tst_discovery` | announce parsing, dedupe by device id not address, ageing, subnet broadcast targets, live-device test gated on `SCAM_DISCOVERY_PORT` |
| `tst_sessionstate` | all eight §24 states, branch ordering, severity mapping — pure, no event loop |
| `tst_userprefs` | defaults, full round-trip, write de-duplication, survival across instances; QSettings redirected to a scratch dir so it never touches the real credential file |
| `tst_devicestatus` | capabilities not requested before a status poll, one control per write, unknown control produces **no** request, 409 surfaces the camera's message, device switch drops and re-asks, reset applies supported defaults in one write, concurrent writes serialise |
| `tst_deviceregistry` | two devices with separate pipelines, one id surviving an address change, announce not stealing the view, unknown id harmless, stale release sparing the active device, port validation |
| `tst_profileengine` | ladder ordered by quality first, every entry cites a measurement, **floors only lowered by a recorded decision**, automatic follows the rule rather than a hard-coded id, three-window shortfall rule, manual mode never advises |
| `tst_diagnostics` | level/category vocabulary, rate limiting, suppression counted and preserved across a limit change, counter spread, ring-buffer bounds |
| `tst_notificationcenter` | ordering, info expiry vs sticky warnings, per-key dedup, dismissal, counting |
| `tst_recorder` | SHA-256 byte-identity of recorded frames, header survival, sidecar flush timing, truncated-tail recovery, double-start refusal, snapshot identity |
| `tst_capture` | in-process MJPEG stub driving the **real** parser and worker: raw bytes survive the stream, snapshot hashes equal the source, recording round-trip equal |

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
