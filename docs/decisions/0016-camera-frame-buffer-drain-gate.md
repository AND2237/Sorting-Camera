# ADR-0016: Frame buffers drain through a gate instead of pinning the camera lock

_Status: accepted 2026-09-30; hardware validation pending (Stage 4 g5)_

## Context

Phase 6 audit **CP-19** / **FW-7**: the camera mutex was held for a frame buffer's
whole lifetime. `camera_fb_get()` took `s_cam_mutex` and did not release it;
`camera_fb_return()` released it. Four sites therefore held the camera lock across
network I/O:

| Site | Path |
|---|---|
| `http_servers.c:278-292` | `snapshot_handler` — one `httpd_resp_send` of a full JPEG |
| `http_servers.c:323-371` | `stream_handler` — up to three `httpd_resp_send_chunk` calls |
| `frame_transport.c:141-162` | `tcp_task` — `send_all` of the header plus the payload |
| `frame_transport.c:247-288` | `udp_task` — payload datagrams |

Everything else that touches the driver queues behind that send: the control
handlers, the config apply, and — worst — `camera_recover()` itself, so the
recovery path that exists to un-wedge a stuck camera could be stuck behind the
stuck camera's own socket. A peer that stops reading costs the lock for
`send_wait_timeout` seconds (5 s, the `HTTPD_DEFAULT_CONFIG` default, applied as
`SO_SNDTIMEO` in `httpd_main.c:91`) or `TX_TIMEOUT_S` seconds (5 s,
`frame_transport.c:131-132`) on every frame, per client.

Two options were rejected before the third was chosen:

- **Shorten the hold but keep it across the send.** Still serialises unrelated
  camera users behind one client's socket, and does nothing for the class.
- **Drop the lock around the send only.** `esp_camera_fb_get()`/`_return` would
  still bracket the send, so a config apply could still `esp_camera_deinit()`
  underneath it. `cam_deinit()` (`cam_hal.c:635-673`) frees every
  `frames[x].fb.buf`, so the client would be mid-send on freed memory.

That second point is the reason a plain lock shortening is not enough.
`esp_camera_fb_get()` (`esp_camera.c:389`) blocks up to `FB_GET_TIMEOUT` = 4000 ms
*inside* the driver, and `esp_camera_fb_return()` early-returns when `s_state` is
NULL — which is set *after* `cam_deinit()` has already freed the buffers. Neither
the driver nor our wrapper can be entered once a teardown starts.

## Decision

Replace "the lock lasts as long as the frame" with an explicit **drain gate** in
`firmware/esp32_cam_stream/main/camera.c`, all guarded by the existing
`s_cam_mutex`:

1. `s_gate_closed` — set while the driver is being torn down or rebuilt.
   `s_gets_in_flight` — an `esp_camera_fb_get()` currently inside the driver.
   `s_fb_held` — a buffer out with a caller.
2. `camera_gate_close()` (taken with the mutex held) waits for any teardown
   already running, sets `s_gate_closed`, then polls until both counters are 0.
   **The mutex is released for every poll iteration**, so consumers are never
   queued behind the teardown and the teardown is never queued behind them.
3. `camera_fb_get()` releases the lock around the driver call (up to 4 s), under
   `s_gets_in_flight`. On return it re-takes the lock: if the gate closed while
   it was inside, it gives the frame straight back and goes round rather than
   returning NULL.
4. `camera_fb_return()` takes the lock it previously assumed, releases the buffer
   through the driver, and decrements `s_fb_held`.
5. `camera_recover()` and `camera_apply_config()` close the gate before touching
   the driver and open it on **every** path out, including the failure and
   rollback paths. A failed close leaves the gate open and returns
   `ESP_ERR_INVALID_STATE` without touching the driver.
6. `CAM_TEARDOWN_TIMEOUT_MS` = 7000 (a little over the 5 s send timeout; the
   4 s driver wait runs concurrently with it, not after it), polled every
   `CAM_GATE_POLL_MS` = 10 ms.

Returning NULL on teardown was explicitly rejected: `STREAM_CAPTURE_FAIL_LIMIT`
is 1 (`app.h:23`), so a single NULL sends the stream handler into
`camera_recover()` and, after two attempts, closes the client. A teardown a
consumer happened to touch must look like a wait, not a failed capture.

**`s_recovering` was removed.** Under the gate it is redundant — any task that
acquires `s_cam_mutex` with the gate open also sees `s_recovering == false` — and
it was actively harmful: an early return during another task's drain-poll would
make the stall watchdog in `app_main.c:66,78` see a non-OK result and
`esp_restart()` spuriously.

**Contract:** a caller must never hold a frame buffer while it asks for a
teardown. No current call site does — the snapshot and stream handlers only
recover when `camera_fb_get()` returned NULL, and the watchdog holds nothing —
but the drain would otherwise wait for itself.

## Consequences

- A stalled client now costs only the teardown that touches it: the drain waits
  up to 7 s, then refuses to touch the driver. Config applies, control commands
  and other clients are no longer serialised behind one socket.
- `esp_camera_fb_get()` runs with no lock held, so two consumers can now capture
  concurrently. That is the driver's design (`fb_count` buffers,
  `CAMERA_GRAB_LATEST`); it was previously over-serialised.
- A teardown can now take up to 7 s instead of the 5 s it could block on the
  mutex. Both are bounded, and a timed-out drain leaves the camera *running*
  rather than half-deinited, which is strictly safer than the old failure mode.
- The lock is held for longer during the deinit/init itself (it always was), so
  that window still blocks `camera_fb_get` — but a waiting consumer now delays
  only itself.
- An outstanding buffer that is never returned no longer wedges the camera
  silently; it surfaces as a `camera teardown: ... frame buffer(s) held` error
  log with both counts.
- **Not addressed:** the `only_quality` fast path in `camera_apply_config`
  (`camera.c`) calls `sensor->set_quality()` with no lock and no gate. It is a
  pre-existing race against a concurrent teardown, unchanged by this work, and is
  recorded as an observation rather than folded into CP-19.

## Evidence

- `idf.py build` (ESP-IDF v5.5.4, `firmware/esp32_cam_stream`): exit 0,
  no warnings in `camera.c`; `esp32_cam_stream.bin` 0xf7710 bytes.
- Send-timeout claim checked against IDF source, not assumed:
  `esp_http_server.h:68` `send_wait_timeout = 5`, applied as `SO_SNDTIMEO` at
  `httpd_main.c:91`; `frame_transport.c:131-132` sets `SO_SNDTIMEO` directly.
- Driver teardown claim checked against the vendored component:
  `esp_camera.c:373` → `cam_hal.c:635-673` frees `frames[x].fb.buf`;
  `esp_camera.c:404` returns early on NULL `s_state`, set after the free.
- Desktop regression after the change: `ctest` **100% passed, 0 failed out of
  12**.
- **Pending:** flash and live-hardware validation (Stage 4 g5) — this build has
  not yet been run on the camera.
