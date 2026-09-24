# Security

_Status: design; implementation in Phase 6._

## Scope (Master Prompt §19)

- Control/API authentication: **required**.
- Video stream encryption: **not** in scope (trusted internal factory LAN). No TLS on ESP32 unless a later measured cost/benefit review justifies it.
- Camera password / Wi-Fi credentials: never committed, never logged.

## Design

1. **Credential storage (device):** operator password stored in NVS as salted hash (e.g. PBKDF2-HMAC-SHA256 or SHA-256+salt at minimum), never plaintext. Provisioned via build-time `config_secrets.h` (git-ignored) → first-boot migration into NVS; production units provisioned per-device.
2. **Login:** `POST /api/v1/auth/login` with client proof over server-issued nonce (challenge–response; password itself never on the wire). Response: session token (random ≥128-bit) + expiry.
3. **Session:** token required on every control endpoint; server-side table of active tokens (bounded); logout invalidates; expiry + sliding refresh.
4. **Replay protection:** nonce is single-use with short TTL; state-changing requests carry timestamp within ±30 s window and token binding.
5. **Rate limiting / lockout:** per-IP exponential backoff after N consecutive auth failures; failures and lockouts logged (without secrets) and surfaced in UI.
6. **Logging hygiene:** tokens/passwords redacted; auth state visible in UI (`unauthenticated / authenticated / locked / expired`).
7. **Stream separation:** MJPEG/TCP/UDP video ports do not grant control authority; control plane stays on authenticated HTTP.

## Non-goals (current)

- Transport encryption of live video.
- Per-frame authorization.
- Internet exposure / port forwarding hardening (LAN product).

## Verification (Phase 8)

Automated tests: token required/rejected/expired paths, replay of captured request must fail, lockout triggers, no secret appears in serial/app logs (grep-based test).
