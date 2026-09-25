# ADR-0007: Transport selection (HTTP MJPEG vs TCP framed vs UDP packetized)

- **Status:** Accepted (2026-09-25, Phase 3 harness results + stability pass)
- **Context:** Phase 3 (`docs/benchmark-plan.md`) required an evidence-based choice
  among the three transports defined in `docs/protocol.md`, measured under identical
  camera configs (640×480/q12 and 1280×720/q12, XCLK 18 MHz, direct softAP link,
  10 s warm-up + 120 s per run, one client). Full numbers: `docs/benchmark-results.md`
  § Phase 3; raw JSON: `benchmarks/results/phase3-20260925-*.json`.
- **Decision:** **TCP framed (port 82, 36-byte SCAM header, CRC32) is the primary
  system transport** for new client code.
  - **HTTP MJPEG (port 81) is retained** as compatibility/diagnostic path — the
    Qt app works on it today, and browsers can view it.
  - **UDP packetized (port 8500) stays implemented but secondary** — revisit only
    if Phase 5 latency work needs a connectionless mode; it is the only transport
    without per-frame latency timestamps in protocol v1.

## Measured basis (2026-09-25, single run per cell)

| | VGA fps (p50) | HD fps (p50) | VGA lat p50 | HD lat p50 | PC CPU | drops |
|---|---|---|---|---|---|---|
| HTTP | 23 | 11 | 36.5 ms | **92.7 ms** | 2.0–2.2 % | 0 |
| TCP | 23 | 9 | **31.1 ms** | 118.6 ms | **1.6 %** | 0 |
| UDP | 17 | 11 | n/a | n/a | 0.9–1.9 % | 1 / 34 frames |

- HTTP vs TCP fps differences track frame-size variation between runs (same
  throughput envelope, ~9–11 fps at HD) — **not a protocol ranking**.
- TCP wins PC CPU and VGA latency; HTTP won HD latency in these runs; UDP is
  fps-poor at VGA (per-fragment overhead) and the only one that loses frames.
- Beyond raw speed, TCP/UDP provide **per-frame CRC + sequence + device
  timestamp** (corruption detection and latency measurement are part of the
  protocol); HTTP multipart has no integrity field — `X-Capture-Us` was added
  only for latency fairness.

## Stability veto (applied per plan)

Not vetoed: 6/6 harness runs with 0 corrupt frames and 0 reboots; kill-while-connected,
AP-disruption and camera-reboot tests all recovered with bounded reconnect and
honest counters. Open issue carried to Phase 5: two unexplained reboots during
interactive debugging (no serial at the time) — see benchmark-results § stability;
must be root-caused (`esp_reset_reason()` + serial) before the 1 h soak gate.

## Consequences

- Qt app gains a TCP framed client next to its existing HTTP MJPEG client
  (receive/assemble off the GUI thread, per AGENTS.md); HTTP remains as fallback.
- `docs/protocol.md` status updated: TCP/UDP framing is normative, not draft.
- UDP implementation + START/keep-alive channel remains available (also used for
  RTT clock sync in the benchmark harness).
- Single-client semantics are accepted for v1 (HTTP and TCP serve one stream;
  UDP single peer): multi-client is explicitly not a ship gate per benchmark-plan.
- Client code must retry once after an immediate post-disconnect `ConnectionReset`
  (observed bounded case in stability pass).
