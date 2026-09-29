# ADR-0017: Config writes are POST-only, with a JSON body and no query string

_Status: accepted 2026-09-30; hardware validation pending (Stage 4 g7)_

## Context

Phase 6 audit **FW-13**: `/api/v1/config` was registered as `HTTP_GET` while the
protocol document said `GET/PUT`. A state-changing endpoint reachable by GET is
not a documentation nit:

- Every write was a URL, so the whole configuration travelled in a field the
  transport may shorten. **FW-10** is the concrete consequence already in the
  audit: `httpd_req_get_url_query_str(req, query, 192)` returns
  `ESP_ERR_HTTPD_RESULT_TRUNC` when the query exceeds 191 bytes
  (`httpd_parse.c:992`), and the old handler only acted on `== ESP_OK`. A
  truncated query therefore fell through to the JSON echo with **200 OK** and
  an unmodified camera — the one response a caller cannot act on. The same
  fall-through answered `200` for a URL with no query at all, which is how a
  bare GET read state.
- A smaller version of the same failure was already present: `char val[16]`
  silently truncated any value longer than 15 characters, and `atoi()` on a
  truncated value produced a number nobody had asked for.
- The device serves `Access-Control-Allow-Origin: *` on every response and has
  no `OPTIONS` handler, so a browser answers any preflight with **405** and
  gives up. That protects the current GET-by-accident rather than by design: a
  page can still issue a *simple* request (GET, or POST with
  `Content-Type: application/x-www-form-urlencoded`, `multipart/form-data` or
  `text/plain`) that carries no preflight and executes before any CORS rule
  gets a chance to refuse it.

## Decision

1. **`GET /api/v1/config` is read-only.** It returns the current state and
   refuses any query string with **400** — including the truncated form, which
   is reported as `config query too long`. There is no third case worth
   answering with anything but 400.
2. **Writes are `POST /api/v1/config` with a 1–512-byte JSON object**, echoed
   back as the resulting state. The handler is registered on the same URI with
   `HTTP_POST`; `esp_http_server` dispatches on URI *and* method
   (`httpd_parse.c:83-143`), so an unmatched method still yields 405 and the
   two handlers cannot catch each other's requests.
3. **`Content-Type` must contain `application/json`, else 415.** This is the
   CSRF control, not hygiene (see below). It is checked before the body is
   read, exactly as the fake camera checks it, so a test cannot pass against
   one and fail against the other.
4. **The body is read and validated in full before anything is applied.** An
   unknown key, a wrong-typed value or an out-of-range number is **400** and
   leaves the camera untouched. The old parser dropped what it did not
   understand — which is how a typo became a setting that appeared to work.
5. **A query string on a POST is 400**, checked only after the body has been
   drained, because answering before the request is consumed leaves the rest of
   it sitting in the socket.
6. The **409 stream gate** (`refuse_while_streaming()`), the atomic apply with
   rollback, the measured-floor and `measured_max_frame_bytes` messages, and
   the self-repair behaviour of the endpoint are unchanged — they move into the
   POST handler verbatim.

### Why a content type and not a CORS rule

`application/json` is not a CORS-safelisted content type, so a browser must
preflight it; an unanswered preflight stops the request before it is sent. The
same is *not* true of `text/plain`, which a hostile page can post with no
preflight at all — so requiring the content type is what makes a cross-origin
write unsendable while the wildcard `Access-Control-Allow-Origin: *` stays on
the responses (dropping the wildcard would break nothing on this device today,
but it would also break any browser tool that reads status or frames).

The residual risk is a request from a non-browser client on a network that can
reach the camera — which is the same as before this change, and is what the
control password and the session token exist for. If a browser viewer is ever
added, the wildcard origin and the absence of an `OPTIONS` handler must be
revisited together rather than one at a time.

## Consequences

- **Breaking client change.** The desktop, both benchmark harnesses and the
  fake camera all moved in the same change: `DeviceStatus` setters now build a
  `QJsonObject` (so `quality` is the number 12, not the string `"12"`),
  `setConfigQuery`/`m_cfgQuery` became `setConfigBody`/`m_cfgBody`,
  `run_phase3.set_config()` and `phase4_matrix.apply_config()` POST JSON, and
  `fake_camera.config_get()` no longer applies anything. Recorded benchmark
  results are untouched — they describe runs already made.
- A URL can no longer carry a configuration, so `curl` examples, bookmarks and
  any shell one-liner must be rewritten as a POST. Nothing in this repository
  still writes through a URL.
- Unknown keys are now an error instead of a no-op, so a firmware/panel version
  skew surfaces as a 400 naming the key rather than as a silent no-op.
- The 415 path means a client that forgot the header gets a precise sentence
  instead of a `415` from the wrong place or a `200` that did nothing. The
  cost is one `httpd_req_get_hdr_value_str` per write.
- **Not addressed:** the endpoint still has no rate limit, and the wildcard
  origin stays on responses. Both are deliberate, and both are recorded above
  rather than left implicit.

## Evidence

- `idf.py build` (ESP-IDF v5.5.4, `firmware/esp32_cam_stream`): exit 0,
  `Project build complete`.
- `ctest` after the desktop change: **100% passed, 0 failed out of 13**. The
  assertions in `tst_devicestatus.cpp` now pin `method == POST`, an exact
  `/api/v1/config` path (so any leaked query string fails the test), the JSON
  body, and that `quality`/`fb_count` arrive as JSON numbers.
- `tools/fake_camera.py` contract smoke against the new handler: 15/15 —
  read-only GET, 400 for a GET or POST with a query string, 415 for a missing
  and for a `text/plain` content type, 400 naming an unknown key, 400 for a
  non-object body, echo and persistence of an accepted write, status unaffected.
- `httpd_parse.c:984-996` read for the truncation behaviour; the GET and POST
  handlers share one `config_state_reply()` so their responses cannot drift.
- **Pending:** flash and live-hardware validation (Stage 4 g7) — this build has
  not yet been run on the camera.
