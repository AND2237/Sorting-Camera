# ADR-0003: LGPLv3-only Qt module policy

- **Status:** Accepted (2026-09-23, confirmed by user)
- **Context:** Product targets commercial/industrial distribution without a paid Qt license (for now). Qt ships two license tiers: LGPLv3/GPLv3 and commercial; several modules (Qt HTTP Server, MQTT, CoAP, Quick3D, Graphs, Lottie, Virtual Keyboard, …) are **GPLv3-only** in open-source builds (Qt 6.11 licensing docs).
- **Decision:** Restrict to LGPLv3-safe modules: Core, Gui, Qml, Quick, Network, Multimedia if justified by benchmark. HTTP/mDNS discovery implemented with **Qt Network (LGPL)** directly, not Qt HTTP Server. FFmpeg avoided unless proven necessary (then LGPL configure). Re-audit before adding any new Qt module.
- **Consequences:** Slightly more DIY for HTTP server-side code (we don't need a server on PC anyway); dynamic linking via standard deploy keeps LGPL compliance straightforward; commercial Qt remains a future option if GPL modules become necessary.
