# ADR-0002: Fresh ESP-IDF baseline (Arduino prototype frozen as reference)

- **Status:** Accepted (2026-09-23, confirmed by user)
- **Context:** Working Arduino prototype exists (dual httpd, MJPEG, GRAB_LATEST, NVS persistence, watchdog) but Master Prompt §3 mandates ESP-IDF + official `esp32-camera` for maintainability/performance control; Arduino example patterns (blocking handlers, opaque generated UI) don't fit the target architecture.
- **Decision:** New ESP-IDF project from scratch in `Sorting_Camera/firmware/`; prototype reused only as behavioral reference (pin map, XCLK 20 MHz lesson, fb_count=3, stall-watchdog idea, sensor control list). Sibling repo never modified.
- **Consequences:** Up-front port cost; full control over tasks/affinity/LwIP/metrics; official component support path; cleaner licensing audit (Apache-2.0 throughout).
