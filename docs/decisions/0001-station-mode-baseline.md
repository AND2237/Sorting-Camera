# ADR-0001: Station + router topology (not AP mode)

- **Status:** Superseded by [ADR-0006](0006-ap-mode-baseline.md) (2026-09-23, user-directed reversal after STA baseline reachieved working L3 during bring-up). Kept for historical context.
- **Context:** Sibling prototype `sorting-cam/ESP32CAM_IndustrialStream` runs ESP32 as SoftAP (192.168.4.1). Master Prompt §5 requires both devices on the same factory router LAN; multi-camera scale-out and existing infra favor station mode.
- **Decision:** Baseline = ESP32 joins router as station; PC on same LAN. AP mode may return later as an optional fallback profile, not the default.
- **Consequences:** Need DHCP resilience + mDNS/UDP discovery (IP is not stable); router quality affects benchmarks (record router/channel in results); matches future multi-camera deployment story.
