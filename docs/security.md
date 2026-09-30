# Security

_Status: implemented in Phase 6; automated coverage as listed under Verification; Phase 8 live verification outstanding._

## Scope (Master Prompt §19)

- Control/API authentication: **required**.
- Video stream encryption: **not** in scope (trusted internal factory LAN). No TLS on ESP32 unless a later measured cost/benefit review justifies it.
- Camera password / Wi-Fi credentials: never committed, never logged.

## Design

1. **Credential storage (device):** operator password stored in NVS as salted hash (PBKDF2-HMAC-SHA256, 8192 iterations, iteration count stored with the record — `auth.c`), never plaintext. Provisioned via build-time `config_secrets.h` (git-ignored) → first-boot migration into NVS; production units provisioned per-device.
2. **Login:** `POST /api/v1/auth/login` with client proof over server-issued nonce (challenge–response; password itself never on the wire). Response: session token (random ≥128-bit) + expiry.
3. **Session:** token required on every control endpoint; server-side table of active tokens (bounded); logout invalidates; expiry + sliding refresh.
4. **Replay protection:** the login challenge is single-use with a 30 s TTL (a captured challenge–response exchange cannot be replayed), and the session token it yields carries the expiry and logout invalidation of item 3. **There is no ±30 s request timestamp window** — earlier revisions of this line claimed one (audit S-2, corrected 2026-09-30); the 30 s is the nonce TTL, not a per-request clock check.
5. **Rate limiting / lockout:** per-IP exponential backoff after N consecutive auth failures; failures and lockouts logged (without secrets) and surfaced in UI.
6. **Logging hygiene:** tokens/passwords redacted; auth state visible in UI (`unauthenticated / authenticated / locked / expired`).
7. **Stream separation:** MJPEG/TCP/UDP video ports do not grant control authority; control plane stays on authenticated HTTP.

## Non-goals (current)

- Transport encryption of live video.
- Per-frame authorization.
- Internet exposure / port forwarding hardening (LAN product).

## Verification

Automated (hermetic suites, run by `scripts\ci.ps1`):

- challenge–response proof vectors; proof changes with nonce/password; no token before sign-in; missing-credential and missing-host are declined (`tst_authclient`).
- the stored credential is never the plaintext password (`tst_credentialstore`: `storedValueIsNotPlaintext`).
- every non-auth control request carries `Authorization: Bearer <token>` once a session exists, and goes out without the header before sign-in; an auth-required camera answers 401 until then (`tst_devicestatus::controlRequestsCarryTheBearerTokenWhenTheCameraRequiresIt`).

Phase 8 (needs the live camera): token expiry end-to-end, replay of a captured state-changing request, lockout/backoff timing on the device (8 failures → lock, exponential backoff), and a grep-based sweep of serial/app logs for secrets. These are the items an earlier revision of this section listed as already automated; they are not.
