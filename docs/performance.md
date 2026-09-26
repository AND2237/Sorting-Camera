# Performance

_Status: targets defined; numbers pending Phase 3/4 measurement._

## Targets (from Master Prompt §1, §34)

- Minimum acceptable sustained: **15 FPS**
- Preferred: **20 FPS+** when hardware permits
- Priority when trading off: **image quality > FPS > latency**
- No unbounded latency growth; latest-frame-wins; drops counted and visible in diagnostics.

## Known physical constraints (external references, not our results)

- OV2640 datasheet (OmniVision): max array rate **UXGA @ 15 fps**, SVGA @ 30 fps — sensor ceiling before any ESP32/Wi-Fi overhead.
- Peer ESP32-CAM HTTP streaming study (arXiv 2505.24081, reference only): ~14 fps VGA, ~8 fps 800×600, ~3.4 fps 1024×768, ~2 fps 720p, ~1.3 fps UXGA on stock-style setup. Indicates **UXGA end-to-end ≥15 fps is unlikely on classic ESP32**; adaptive fallback to SVGA/XGA-class resolutions will probably be required. Our Phase 4 matrix confirmed this - UXGA failed the floor at every quality; the measured ladder is in docs/benchmark-results.md (Phase 4 section) and docs/decisions/0008-camera-operating-ladder.md.

## Where the bottleneck can live

Sensor readout → DVP/DMA → JPEG buffer in PSRAM → CPU copy → Wi-Fi/TCP stack → direct softAP link → PC NIC → assembler → JPEG decode → scene-graph upload → display. Every stage gets instrumented before any optimization claim.

## Instrumentation points

Device: capture duration, fb size, tx bytes/FPS, heap/PSRAM, RSSI (see `docs/benchmark-plan.md`).
PC: recv FPS/bytes, decode ms, render FPS, drop counters, stage latencies, CPU/GPU/memory.

## Reporting rules

Every published figure must cite firmware+app version, full config, PC hardware, date, duration. Median + p95. "Not measured" is a valid and required answer when true.
