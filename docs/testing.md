# Testing

_Status: harnesses added incrementally from Phase 2 onward._

## Levels

1. **Host unit tests (desktop):** protocol header encode/decode, CRC, frame reassembly (incl. truncated/oversized/malformed input), latest-wins drop policy, metric math (FPS/bitrate/latency aggregation), config validation, recorder byte-identity (write → read → SHA-256 equal), discovery packet parse. Framework: Qt Test or Catch2 (pick one and stay with it; Qt Test preferred — no new dependency).
2. **Firmware host-buildable tests:** pure functions (header build, CRC, config apply validation) compiled on host where feasible; on-device smoke via Unity/`idf.py -T` if enabled.
3. **Integration (manual/semi-auto scripts):** spin app against firmware (or a PC-side MJPEG/TCP/UDP **fake camera** server under `tools/` for CI without hardware) → verify connect, auth, stream, snapshot, record, reconnect.
4. **Soak (Phase 8):** ≥1 h continuous stream while toggling: resolution changes, snapshot, record start/stop, PC Wi-Fi leave/rejoin (AP disruption), client kill/reconnect, camera reboot. Watch: memory trend, drop counters, stale-frame age, UI responsiveness, thread safety (TSAN on host tests where possible).

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
