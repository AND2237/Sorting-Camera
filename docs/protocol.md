# Protocol

_Status: candidates under benchmark (Phase 3). Custom framing spec below is **draft** until transport is selected._

## Control plane (decided)

HTTP/1.1 + JSON over TCP port 80 on the camera (separate from video port where the transport requires it). All control endpoints require an authenticated session token (see `docs/security.md`).

Endpoints (initial):

| Method/path | Purpose |
|---|---|
| `GET /api/v1/status` | device info, camera state, metrics snapshot (rate-limited fields ok) |
| `POST /api/v1/auth/login` | challenge/response → session token |
| `POST /api/v1/auth/logout` | invalidate token |
| `GET/PUT /api/v1/config` | resolution, JPEG quality, sensor controls (only controls the driver reports as supported). Implemented as `GET /api/v1/config[?framesize=&quality=&xclk=]`: bare GET returns current state; with params it applies them and returns the new state. Returns **409 Conflict** if any param is sent while a stream client is connected (config changes require stream disconnect — live change wedged the control server, see benchmark-results). |
| `GET /api/v1/capabilities` | supported resolutions, control ranges, fw/protocol versions |

## Video plane candidates

### A. HTTP multipart MJPEG (baseline)

Standard `multipart/x-mixed-replace` stream. Zero framing invention; easiest correctness; known issues: head-of-line blocking, boundary parsing cost, single client per handler task, no per-frame metadata beyond `Content-Length`.

### B. TCP framed JPEG (candidate)

Single persistent TCP connection. Every frame:

```
offset  size  field
0       4     magic "SCAM"
4       1     protocol version (u8)
5       1     header length (u8) — allows future extension
6       2     device ID (u16, assigned/stable within fleet; low byte = uid-derived)
8       4     frame sequence (u32, wraps)
12      8     device timestamp (u64, µs since boot)
20      2     width (u16)
22      2     height (u16)
24      1     pixel format (u8; 0 = JPEG)
25      1     compression (u8; 0 = JPEG baseline)
26      4     payload length (u32)
30      2     flags (u16; bit0 = keyframe/resume marker, bit1 = config-changed, rest reserved 0)
32      4     CRC32 of payload (u32)
36      N     JPEG payload
```

Field justifications:

- `magic + version + header_len` — unambiguous resync, explicit compatibility (req. §7, §39).
- `device_id` — multi-camera demux without separate connections (req. §27).
- `seq` — loss/jitter measurement + stale detection (req. §8, §13).
- `timestamp` — device-side latency decomposition (req. §14).
- `width/height/format/compression` — self-describing frame, no out-of-band config to desync (req. §7).
- `payload length` — TCP stream reassembly (req. §7).
- `flags` — sparse needs only (resume after reconnect, notice of mid-stream config change); reserved bits keep it extensible without breaking v1 parsers.
- `CRC32` — detect corruption on memory-constrained path cheaply; TCP already checksums, so this also covers device-memory DMA glitches (measured use will decide keep/flag-disable).

Total overhead 36 B/frame — negligible vs JPEG payload sizes; no field added "because it sounds useful".

### C. UDP packetized JPEG (candidate)

Frame split into fixed-MTU (1400 B) datagrams; per-datagram header: frame seq, fragment index/count, frame CRC. Incomplete frame ⇒ drop whole frame (counted). Optional later: FEC — only if benchmarks show loss is a real problem on this LAN (req. §43).

Tradeoffs to measure: CPU on ESP32, loss behavior on the direct AP link, multi-client scaling, stale-frame behavior, reconnection simplicity.

## Versioning & compatibility

- `protocol version` in every custom frame/header; control API uses `/api/v1/` prefix.
- Desktop rejects major-version mismatch with a clear UI error; minor additions are backward-compatible (header_len / flags reserved bits).
- Firmware version, protocol version, app version each reported in status/discovery and recorded in benchmark results (req. §39, §13).

## Freshness policy (all transports)

Latest-complete-frame-wins end to end: if decode/render falls behind, older frames are dropped and counted (`frames_dropped`), never queued without bound (req. §8).
