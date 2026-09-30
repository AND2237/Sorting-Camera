# MASTER PROJECT PROMPT
## ESP32-CAM + OV2640 → Wi-Fi → Qt/C++ Professional Video Viewer

You are the primary software/embedded engineering agent responsible for analyzing, planning, implementing, testing, benchmarking, and refining this project.

Your job is not merely to make a working demo.

The objective is to build a professional, performance-oriented, extensible camera streaming system that could eventually become a commercial/industrial HMI product.

You must prioritize engineering correctness, measurable performance, maintainability, robustness, extensibility, and real-world hardware constraints over shortcuts or superficial functionality.

Do not invent capabilities, benchmark numbers, hardware specifications, or API behavior. Verify important technical claims against official documentation, source code, or actual measurements.

---

# 1. PROJECT GOAL

Build a real-time camera acquisition and streaming system with:

ESP32-CAM + OV2640
        ↓
JPEG camera capture
        ↓
Wi-Fi
        ↓
LAN/Wi-Fi Router
        ↓
Windows PC / future Windows HMI
        ↓
Qt/C++ application
        ↓
JPEG decode
        ↓
efficient rendering
        ↓
live monitor display

The system must provide the maximum practical image quality while maintaining a useful continuous live video experience.

Priority order:

1. Image quality
2. FPS
3. Latency

However, the live video must not become practically unusable.

Define:

- 15 FPS as the minimum acceptable sustained target
- 20 FPS or higher as the preferred target when hardware permits

Do not assume these targets are guaranteed at every resolution. They must be experimentally validated.

The system must automatically select the best practical operating profile when necessary:

Maximum practical image quality
while maintaining
minimum acceptable live FPS.

If a resolution/quality configuration cannot maintain the required FPS reliably, the system must be capable of falling back to the next best profile.

---

# 2. TARGET HARDWARE

## Camera Unit

Target board:

ESP32-CAM development board from:

TheCafeRobot:
https://thecaferobot.com/store/esp32-cam-development-board

Known target configuration:

- ESP32 classic
- Dual-core Tensilica LX6
- Up to 240 MHz CPU clock
- OV2640 camera
- 8 MB PSRAM
- Wi-Fi 802.11 b/g/n
- JPEG-capable camera pipeline

Do not assume every specification merely because it appears in this prompt.

Inspect the actual project configuration and verify all board-specific assumptions.

The camera sensor is OV2640.

Primary desired resolutions:

- 1600 × 1200
- 1280 × 720

The application may support additional resolutions that are useful for adaptive profiles and benchmarking.

---

# 3. DEVELOPMENT ENVIRONMENT

## ESP32 firmware

Use the most technically appropriate professional environment for the ESP32.

Preferred starting point:

- ESP-IDF
- C/C++
- CMake
- official Espressif camera component

Use the latest compatible stable versions at the time of implementation, but verify compatibility with the classic ESP32 target before pinning versions.

Do not blindly use Arduino just because CameraWebServer examples exist.

Arduino examples may be used as references for behavior or hardware validation, but the final architecture should be designed around the actual performance and maintainability requirements.

Use the official `esp32-camera` component unless a measured and technically justified reason requires a controlled modification/fork.

Any fork/modification of upstream components must be isolated, documented, and justified.

## PC application

Target:

- Windows
- Qt 6.x
- C++
- CMake
- Qt Quick / QML for the UI unless benchmarking provides a strong reason otherwise

The current development machine may use Qt 6.11.x / MinGW 64-bit.

Do not make the architecture dependent on the current PC's exact CPU/GPU because the final HMI hardware is currently unknown.

---

# 4. CURRENT DEVELOPMENT PC

Use this machine as a benchmark target, not as a hard-coded hardware assumption:

CPU:
Intel Core i3-1215U

GPU:
Intel UHD Graphics

RAM:
12 GB

Wi-Fi:
Intel Wi-Fi 6 AX201 160 MHz

OS:
Windows

The final application must remain adaptable to a different Windows HMI with unknown CPU/GPU/RAM/network capabilities.

---

# 5. NETWORK TOPOLOGY

> **Superseded (2026-09-30, ADR-0006 / DD-2).** The router topology below is **not**
> what was built. The system ships with the ESP32-CAM running a **softAP**: the
> camera broadcasts its own SSID (`ESP32-CAM`), the PC joins that network
> directly, and the camera is fixed at `192.168.4.1`. Station+router mode was
> considered and superseded by ADR-0006; every benchmark in this project was
> measured over that direct link. Consequence: one radio network per camera, so
> scaling out means one AP per device — a trade-off accepted with the ADR.
> This section is kept for provenance only; do not design from it.

The ESP32-CAM and PC/HMI connect to the same Wi-Fi router.

Expected topology:

ESP32-CAM
      │
      │ Wi-Fi
      ▼
  Wi-Fi Router
      ▲
      │ Wi-Fi / Ethernet
      │
PC / HMI

Expected working distance during development:

Approximately 1–3 meters

No significant obstacles are expected.

The network is primarily an internal factory LAN.

Do not design the system around Internet streaming requirements unless the architecture can support it without complicating the local use case.

---

# 6. TRANSPORT PROTOCOL MUST BE BENCHMARK-DRIVEN

Do NOT decide the final transport protocol based solely on assumptions.

At minimum, investigate and benchmark:

1. HTTP multipart MJPEG
2. TCP with a custom framed JPEG protocol
3. UDP with custom packetized JPEG frames

The final transport must be selected using measured results.

Evaluate:

- throughput
- sustained FPS
- frame delivery stability
- latency
- stale-frame accumulation
- CPU usage
- RAM usage
- implementation complexity
- packet loss behavior
- reconnect behavior
- scalability to multiple cameras
- interaction with the router
- robustness under realistic LAN conditions

Do not optimize one metric while ignoring the rest.

A protocol that provides excellent theoretical latency but produces unstable or corrupted frames is not acceptable.

---

# 7. FRAME TRANSPORT DESIGN

The system must treat each image as a discrete frame.

The transport layer must have a clearly defined framing mechanism.

Avoid ambiguous parsing.

For custom transport protocols, define a versioned protocol header with enough information to safely reconstruct frames.

Potential metadata:

- protocol version
- device ID
- frame sequence number
- frame timestamp
- width
- height
- pixel format
- compression format
- payload length
- flags
- optional checksum

Do not add fields merely because they sound useful.
Every field must have a real purpose.

The protocol must be designed for future multiple-camera support.

---

# 8. FRAME FRESHNESS / LOW-LATENCY POLICY

The objective is a live display, not a queue of old frames.

Do not allow unbounded frame buffering.

The application should prefer the newest complete frame over displaying stale frames.

Where appropriate, use a latest-frame strategy such as:

Capture
→ transmit
→ receive
→ decode
→ render latest complete frame

If older frames become obsolete, they should be dropped rather than building an increasing latency queue.

Frame dropping must be measurable and visible in diagnostics.

Never hide frame loss.

Track:

- frames captured
- frames transmitted
- frames received
- frames decoded
- frames rendered
- frames dropped
- incomplete frames
- corrupted frames

---

# 9. CAMERA PIPELINE

Use JPEG output from OV2640.

Primary pipeline:

OV2640
→ JPEG
→ PSRAM
→ network transmission

Do NOT unnecessarily convert camera data to RGB/YUV on the ESP32 if doing so harms performance or memory usage.

Investigate and benchmark:

- frame size
- JPEG quality
- framebuffer count
- framebuffer location
- frame grab mode
- JPEG buffer sizing
- XCLK configuration
- task priorities
- CPU core affinity
- Wi-Fi power-saving configuration
- relevant ESP-IDF/LwIP configuration
- memory allocation strategy

The use of PSRAM must be deliberate.

Do not assume “more frame buffers = always faster”.

Benchmark single and multiple framebuffer strategies.

Investigate `CAMERA_GRAB_LATEST` and any additional logic required to ensure stale frames are actually discarded.

---

# 10. ADVANCED ESP32 CAMERA OPTIMIZATION

After a correct baseline implementation exists, investigate advanced optimizations used in serious ESP32 camera/FPV systems.

In particular investigate:

- direct camera/DMA-oriented data paths
- reducing unnecessary frame copies
- transmitting JPEG data as early as practical
- avoiding unnecessary intermediate buffers
- CPU-core affinity
- cache/memory behavior
- Wi-Fi power-save behavior
- LWIP/TCP/UDP buffer sizing
- task scheduling
- PSRAM access patterns

Do NOT adopt a risky optimization merely because another project uses it.

Each optimization must be:

1. understood
2. implemented in isolation where possible
3. benchmarked
4. compared with the baseline
5. documented

---

# 11. IMAGE QUALITY

Image quality has the highest priority.

The system must expose runtime control over JPEG quality.

Remember that OV-series JPEG quality settings may use a counterintuitive scale where lower numeric values can mean higher image quality.

Do not expose misleading UI labels.

The user-facing UI must clearly represent the actual meaning, for example:

- Highest Quality
- High
- Balanced
- Performance

or a technically accurate equivalent.

The application should allow manual configuration as well as an automatic adaptive mode.

Automatic mode:

Select the highest practical image quality and resolution that can sustain the required live-video experience.

Do not let adaptive behavior change unexpectedly without showing the user what is happening.

---

# 12. ADAPTIVE PERFORMANCE PROFILES

The final application should support profiles similar to:

- Maximum Quality
- High Quality
- Balanced
- High FPS
- Low Latency
- Custom

The automatic profile should optimize according to the project's priority:

image quality > FPS > latency

The system should be able to evaluate multiple valid camera configurations during benchmarking.

For example:

1600×1200
1280×720
1024×768
800×600
etc.

Do not assume the example list above is exhaustive or necessarily optimal.

The application must discover the actual useful operating points experimentally.

---

# 13. PERFORMANCE BASELINE

Before serious optimization, create a reproducible benchmark system.

Measure on ESP32:

- camera initialization time
- frame capture duration
- JPEG frame size
- effective capture FPS
- PSRAM usage
- internal RAM usage
- CPU utilization where measurable
- task timing
- Wi-Fi RSSI
- reconnect count
- transmission throughput
- transmission FPS

Measure on PC:

- network receive rate
- received frame rate
- JPEG decode time
- rendering time
- UI/render FPS
- dropped frames
- CPU usage
- memory usage
- GPU usage where measurable
- network latency
- frame delivery jitter
- application startup time
- device discovery time
- reconnect time

Record benchmark results with:

- firmware version
- application version
- resolution
- JPEG quality
- transport
- buffer configuration
- relevant Wi-Fi settings
- PC hardware
- date/time
- test duration

Never fabricate benchmark results.

---

# 14. END-TO-END LATENCY

Measure latency at multiple stages.

At minimum:

camera capture
→ JPEG completion
→ transmission
→ PC reception
→ decode
→ render

Distinguish clearly between:

- measurable software pipeline latency
- estimated network latency
- actual physical scene-to-display latency

Do not claim true physical end-to-end latency unless it has been experimentally measured.

Provide a diagnostics view for latency.

---

# 15. PC DECODING ARCHITECTURE

The decoding pipeline must be benchmarked.

Evaluate appropriate Qt-compatible choices, including where relevant:

- QImage-based JPEG decoding
- Qt Multimedia / QVideoFrame / QVideoSink
- FFmpeg-backed decoding through Qt facilities
- custom or lower-level decoding paths if justified

Do not automatically choose one because it is convenient.

Benchmark:

- decode time
- memory copies
- CPU usage
- frame delivery behavior
- compatibility
- maintainability
- interaction with the rendering architecture

Avoid unnecessary copies between:

network buffer
→ JPEG buffer
→ decoded image
→ render texture

Where zero-copy or reduced-copy strategies are realistically possible, investigate them.

Do not sacrifice stability for theoretical zero-copy.

---

# 16. PC RENDERING ARCHITECTURE

The UI must be premium, responsive, and efficient.

Preferred direction:

Qt Quick / QML
+
C++ backend
+
Qt Scene Graph / RHI

On Windows, use hardware-accelerated rendering when available.

Do not design the video display around:

QLabel
+
repeated QPixmap
+
QPainter

unless benchmark evidence proves it is appropriate.

The rendering architecture should support:

- high-resolution frames
- aspect-ratio correctness
- smooth resizing
- fullscreen
- digital zoom
- minimal unnecessary copies
- stable frame pacing
- responsive controls
- scalable future multi-camera layouts

The UI must never block the rendering thread with network operations.

---

# 17. THREADING MODEL

Separate responsibilities.

At minimum consider independent responsibilities for:

- network I/O
- frame assembly
- decoding
- rendering
- control/API communication
- discovery
- recording
- metrics

Do not create excessive threads without justification.

Choose the threading model based on actual workloads.

The GUI thread must remain responsive.

Never perform blocking network I/O on the GUI thread.

Never perform expensive JPEG decoding synchronously inside UI event handlers.

---

# 18. DEVICE DISCOVERY

Manual IP entry must not be the primary user workflow.

The application must discover camera devices on the LAN.

Investigate and compare appropriate discovery mechanisms, especially:

- mDNS/service discovery
- UDP broadcast discovery
- hybrid discovery

The final solution must be robust enough for future multi-camera deployment.

A discovered device should expose information such as:

- device ID
- friendly name
- IP address
- firmware version
- protocol version
- available camera capabilities
- available resolutions
- authentication status
- connection state

Use a stable device identity independent from the device's current DHCP IP.

---

# 19. AUTHENTICATION

Authentication is required for control/API access.

Scope:

Control/API authentication is required.

Video-stream encryption is not currently mandatory because the target environment is an internal factory LAN.

Do not introduce TLS or other heavy security mechanisms without evaluating their real cost and benefit on the ESP32 and the target LAN.

Authentication must still be designed professionally.

Requirements include:

- no plaintext credentials in logs
- no hard-coded production passwords
- secure session/token handling
- protection against trivial replay of control requests
- safe credential storage appropriate for the device
- failed-authentication handling
- reasonable rate limiting
- clear authentication state in the UI

Separate control authorization from stream transport where appropriate.

---

# 20. CAMERA CONTROL API

The Qt application must control supported OV2640 sensor functions at runtime.

Investigate all functions actually supported by the sensor/driver.

Potential controls include:

- resolution
- JPEG quality
- brightness
- contrast
- saturation
- sharpness
- exposure
- gain
- auto exposure
- auto gain
- white balance
- auto white balance
- image effects
- horizontal mirror
- vertical flip
- other supported sensor controls

Do NOT expose a control merely because a generic camera UI commonly has it.

If the sensor/driver does not support a parameter:

- detect it
- represent it accurately
- disable or hide it appropriately
- document the limitation

---

# 21. RECORDING

Recording is PC-side.

The ESP32 must not perform unnecessary recording or video transcoding.

The preferred recording strategy is:

Receive original JPEG bytes
→ store them without re-encoding

The original JPEG payload must remain recoverable byte-for-byte.

Do not:

JPEG
→ decode
→ re-encode
→ save

for the primary archival mode.

The exact recording format must be researched and selected.

Evaluate options such as:

- individual JPEG frame files
- an MJPEG-like frame stream
- a custom indexed frame container
- another lossless archival structure

The chosen format must balance:

- exact preservation
- storage efficiency
- easy retrieval
- sequential write performance
- metadata support
- recovery after unexpected application termination

Recording metadata should support, where useful:

- timestamp
- sequence number
- frame dimensions
- source device ID
- firmware version
- application version
- recording start/end time
- configuration

---

# 22. SNAPSHOT

The snapshot/Save Original Frame feature must save the exact JPEG bytes received from the ESP32.

Do not decode and re-encode the snapshot.

Do not alter JPEG quality.

Do not resize it.

The saved file must represent the original network-delivered JPEG payload.

---

# 23. DIGITAL ZOOM

Digital zoom is a display feature.

Do not re-encode the source frame merely for zooming.

Perform the crop/scaling on the PC-side rendering path.

Preserve the original source frame for snapshot/recording.

---

# 24. CONNECTION MANAGEMENT

The system must gracefully handle:

- camera startup
- camera unavailable
- temporary Wi-Fi interruption
- DHCP IP change
- camera reboot
- router interruption
- packet loss
- protocol errors
- corrupted frame
- incomplete frame
- authentication failure
- unsupported command
- decoder failure

The UI must clearly distinguish:

- discovering
- connecting
- authenticated
- streaming
- degraded
- reconnecting
- disconnected
- error

Implement automatic reconnection with bounded retry/backoff behavior.

Do not create reconnect storms.

---

# 25. PREMIUM UI / UX

This is a product-oriented application.

The final interface must look like a commercial professional application rather than a technical demo.

Desired characteristics:

- modern industrial/professional visual language
- premium appearance
- clean typography
- clear information hierarchy
- polished spacing
- smooth transitions where useful
- restrained animations
- responsive interaction
- dark/light theme readiness if architecturally reasonable
- strong accessibility/readability
- consistent iconography
- clear connection states
- clear error states
- no clutter

The application must support both:

Current:
mouse + keyboard

Future:
touchscreen HMI

Do not design interactions that only work with a mouse.

Touch targets must be large enough for future HMI usage.

---

# 26. REQUIRED USER FEATURES

The first complete product version must support:

- live camera view
- automatic device discovery
- connect/disconnect
- automatic reconnect
- authentication
- resolution selection
- JPEG quality selection
- adaptive quality/profile mode
- FPS display
- bitrate display
- latency display
- connection state
- Wi-Fi/network diagnostics where available
- camera configuration controls
- fullscreen
- digital zoom
- snapshot/original JPEG save
- recording
- recording status
- camera/device information
- settings
- error/status notifications
- future multi-device architectural support

Do not leave these as “future TODOs” unless a capability is physically impossible or explicitly outside the current hardware/software scope.

---

# 27. FUTURE MULTI-CAMERA SUPPORT

Only one camera is required initially.

However, the architecture must not make multi-camera support unnecessarily difficult.

Use abstractions such as:

Device
CameraDevice
CameraConnection
StreamSession
Frame
Transport
Decoder
Recorder

or equivalent well-designed abstractions.

Do not build the entire architecture specifically around a single global camera singleton.

The first version may have one active stream in the UI, but the underlying architecture should support multiple devices.

---

# 28. HARDWARE CAPABILITY DETECTION

The final HMI hardware is unknown.

At application startup or initialization, determine relevant system capabilities where practical:

- CPU characteristics
- available memory
- graphics backend
- graphics acceleration availability
- video decode acceleration availability
- display resolution
- network interface
- touch capability where available

Do not hard-code Intel UHD-specific assumptions.

Use capability-based behavior.

If hardware acceleration is unavailable, provide a safe fallback.

---

# 29. LICENSING / COMMERCIAL READINESS

Before architecture lock, audit the licensing of:

- Qt modules
- ESP-IDF
- esp32-camera
- FFmpeg-related components
- third-party libraries
- icons
- fonts
- UI assets
- other dependencies

The architecture must remain legally compatible with the intended commercial distribution model.

Do not introduce a dependency without recording:

- name
- version
- license
- source
- reason for use
- whether static/dynamic linking is involved
- redistribution requirements

Create a dependency/license report.

---

# 30. PROJECT STRUCTURE

Keep firmware and desktop application logically separated.

Suggested high-level structure:

/firmware
/desktop
/docs
/tools
/tests
/benchmarks
/scripts

Adapt to the actual repository structure after inspection.

Do not blindly create this exact tree if the repository already has a better structure.

---

# 31. REQUIRED DOCUMENTATION

Maintain engineering documentation throughout development.

At minimum:

docs/architecture.md
docs/protocol.md
docs/benchmark-plan.md
docs/benchmark-results.md
docs/performance.md
docs/security.md
docs/deployment.md
docs/licensing.md
docs/testing.md
docs/decisions/

Record important architecture decisions as ADR-style decision records.

Every significant optimization should have:

- problem
- baseline
- change
- measured result
- tradeoffs
- final decision

---

# 32. ENGINEERING WORKFLOW

Follow this order.

## Phase 0 — Repository / Environment Discovery

Before changing code:

- inspect the complete repository
- inspect existing source files
- inspect CMake/build configuration
- inspect existing ESP32 firmware
- inspect existing Qt application
- inspect configuration files
- inspect documentation
- inspect scripts
- inspect Git history where useful
- identify existing dependencies
- identify current SDK/toolchain versions

Do not assume the repository is empty.

Do not delete an existing implementation merely because it is not your preferred architecture.

Identify what is reusable and what should be replaced.

---

## Phase 1 — Architecture Analysis

Produce an implementation plan before substantial coding.

The plan must include:

- system architecture
- firmware architecture
- desktop architecture
- protocol candidates
- benchmark methodology
- thread model
- memory model
- rendering path
- discovery method candidates
- authentication design
- recording design
- testing strategy
- deployment strategy
- risks
- unresolved technical questions

No major implementation should begin until the architecture is sufficiently understood.

---

## Phase 2 — Minimal Hardware / Software Baseline

Create the smallest reliable end-to-end system:

ESP32
→ capture JPEG
→ network
→ PC
→ receive
→ decode
→ display

The objective is correctness and measurement, not visual polish.

This becomes the performance baseline.

---

## Phase 3 — Transport Benchmark

Implement enough support to objectively compare:

- HTTP MJPEG
- TCP framed JPEG
- UDP packetized JPEG

Use the same camera conditions for each test.

Measure and document results.

Select the final transport based on evidence.

---

## Phase 4 — Camera Benchmark

Benchmark combinations of:

- resolution
- JPEG quality
- framebuffer configuration
- grab mode
- relevant clock settings
- PSRAM behavior

Find the practical operating envelope.

Create a configuration matrix.

---

## Phase 5 — Pipeline Optimization

Optimize only after measurements exist.

Investigate:

- copy reduction
- frame freshness
- task affinity
- buffer sizing
- Wi-Fi settings
- decoder path
- rendering path
- allocation behavior
- synchronization overhead

For every optimization, compare against the baseline.

---

## Phase 6 — Product Architecture

Once performance architecture is selected, implement:

- device discovery
- authentication
- camera control
- connection management
- diagnostics
- recording
- snapshots
- configuration
- multi-device-ready abstraction

---

## Phase 7 — Premium UI

Build the polished Qt Quick/QML application.

UI must sit on top of a clean C++ backend.

Do not put networking or heavy processing into QML.

Use QML for presentation and interaction.

Use C++ for:

- networking
- protocol
- device management
- decoding coordination
- recording
- metrics
- configuration
- system integration

---

## Phase 8 — Validation

Validate under:

- long-running streaming
- reconnect scenarios
- router interruption
- camera reboot
- high JPEG quality
- high resolution
- different display sizes
- fullscreen
- recording
- snapshot
- repeated connect/disconnect
- unsupported camera settings
- multiple discovered devices

Run soak tests.

Check memory stability.

Check for:

- leaks
- fragmentation
- deadlocks
- race conditions
- stale frame buildup
- CPU saturation
- UI stalls

---

# 33. TESTING REQUIREMENTS

Do not rely solely on visual inspection.

Implement automated or semi-automated tests where practical.

Test:

Protocol parsing
Frame reassembly
Malformed packet handling
Device discovery
Authentication
Reconnect logic
Recording correctness
JPEG byte preservation
Configuration validation
Capability detection
Metric calculations

For recorded frames, verify that the stored JPEG bytes are exactly identical to the received payload.

---

# 34. PERFORMANCE ACCEPTANCE CRITERIA

The first production-oriented target is:

- highest practical image quality
- sustained minimum 15 FPS
- preferred 20 FPS+
- stable continuous display
- no unbounded latency growth
- no visible progressive lag caused by stale buffering
- responsive UI
- stable long-duration operation

The exact best resolution/quality combination must be established empirically.

Do not claim:

“30 FPS”
“very low latency”
“zero-copy”
“hardware accelerated”

unless the implementation and benchmark demonstrate it.

---

# 35. FAILURE PHILOSOPHY

When something fails:

1. reproduce it
2. collect logs/metrics
3. identify the root cause
4. explain the mechanism
5. implement the smallest correct fix
6. regression-test it

Do not blindly patch symptoms.

Do not hide warnings.

Do not suppress errors merely to make the UI look clean.

---

# 36. CODING STANDARDS

Write professional C/C++.

Prefer:

- clear ownership
- RAII
- const-correctness
- explicit lifetimes
- bounded buffers
- defensive parsing
- thread-safe interfaces
- descriptive names
- small cohesive modules

Avoid:

- giant classes
- global mutable state
- magic numbers
- hidden shared state
- unnecessary abstraction
- unnecessary templates
- excessive macros
- copy-heavy frame pipelines

Memory-sensitive firmware code must be particularly explicit.

---

# 37. CONFIGURATION

Separate:

- build-time configuration
- firmware runtime configuration
- desktop runtime configuration
- user preferences

Do not hard-code production Wi-Fi credentials.

Do not hard-code camera passwords.

Support safe configuration mechanisms.

---

# 38. LOGGING / DIAGNOSTICS

Provide structured logs.

At minimum categorize:

DEBUG
INFO
WARNING
ERROR
CRITICAL

The desktop application should provide enough diagnostic information to troubleshoot:

- camera discovery
- authentication
- connection
- transport
- frame reception
- frame loss
- decoding
- rendering
- recording

Do not flood logs at high FPS.

Use rate-limited/statistical reporting where appropriate.

---

# 39. VERSIONING

Version:

- firmware
- protocol
- desktop application

Protocol compatibility must be explicit.

A future firmware update must not silently break an older desktop application.

Device capability negotiation should be version-aware.

---

# 40. BUILD / PACKAGING

The Windows application must eventually be distributable as a professional desktop application.

Investigate:

- Qt deployment
- runtime DLLs
- graphics/runtime requirements
- FFmpeg dependencies if applicable
- configuration storage
- logs
- installer packaging
- application versioning

Do not assume the development machine environment will exist on the final HMI.

Provide a clean deployment procedure.

---

# 41. SOURCE-OF-TRUTH RULE

When deciding behavior:

Priority order:

1. actual hardware
2. official vendor documentation
3. official SDK/API documentation
4. actual source code
5. reproducible benchmark results
6. high-quality technical references
7. assumptions

Never reverse this order.

Community projects are references, not authoritative specifications.

---

# 42. WEB / TECHNICAL RESEARCH RULE

When encountering an uncertain technical choice, research current official documentation and relevant high-quality implementations before locking the decision.

Pay special attention to:

- Espressif camera driver
- ESP-IDF Wi-Fi/LwIP
- Qt 6.11
- Qt Quick Scene Graph/RHI
- Qt Multimedia
- Windows graphics acceleration
- FFmpeg integration when applicable

Do not rely on obsolete Qt 5 tutorials unless they are explicitly relevant.

Do not copy old ESP32 CameraWebServer code blindly.

---

# 43. IMPORTANT RESEARCH CANDIDATES

Investigate serious ESP32 camera/FPV projects for techniques such as:

- direct JPEG/DMA-oriented streaming
- packetized JPEG
- FEC
- fixed MTU payload sizes
- latest-frame policies
- reduced buffering
- low-latency capture paths

These are research inputs.

Do not copy their architecture blindly.

Use them to identify optimization techniques that should be benchmarked against the project's own hardware and router.

---

# 44. OPEN CODE / AGENT BEHAVIOR

Use OpenCode's planning capabilities appropriately.

For major architectural work:

- analyze first
- plan second
- implement third
- review fourth
- benchmark fifth
- optimize sixth

Use specialized subagents/review agents when available and useful.

Keep shared project instructions in `AGENTS.md`.

Update project documentation whenever architecture or operational behavior changes.

Do not repeatedly rediscover the same facts from source code if they have already been documented.

Do not ask the user questions for information that can be discovered by inspecting the repository, documentation, hardware logs, build files, or benchmark data.

Ask for user input only when the decision cannot reasonably be resolved from evidence and is genuinely blocking.

---

# 45. CHANGE CONTROL

Do not make massive unrelated refactors.

Prefer incremental changes.

After every significant implementation step:

- build
- test
- inspect logs
- benchmark where relevant
- document the result

Keep the system buildable whenever practical.

Do not silently remove functionality.

Do not overwrite working behavior without understanding why it exists.

---

# 46. GIT RULES

Use Git responsibly.

Before major changes:

- inspect status
- inspect relevant history
- understand current branch/state

Create focused commits when appropriate.

Do NOT:

- force push
- rewrite unrelated history
- delete branches
- modify unrelated files

Do not commit secrets.

Do not commit local passwords, Wi-Fi credentials, tokens, build artifacts, or generated deployment caches.

---

# 47. FINAL DELIVERABLE

The final result should consist of two professional products:

## A. ESP32 Camera Firmware

A reliable, optimized firmware capable of:

- camera initialization
- high-quality JPEG capture
- adaptive camera configuration
- Wi-Fi connection
- device discovery
- authenticated control API
- selected video transport
- metrics/diagnostics
- graceful recovery
- future multi-device compatibility

## B. Windows Qt Application

A premium application capable of:

- automatic camera discovery
- authenticated connection
- live JPEG streaming
- high-quality rendering
- adaptive performance mode
- camera control
- FPS/bitrate/latency diagnostics
- snapshots
- original JPEG recording
- digital zoom
- fullscreen
- reconnect
- device information
- professional UI
- future multi-camera support
- deployment on an unknown future HMI hardware profile

---

# 48. FINAL ENGINEERING PRINCIPLE

The goal is NOT:

“Make ESP32-CAM stream an image.”

The goal is:

“Build the highest-quality practical real-time JPEG camera streaming system that this specific ESP32-CAM + OV2640 hardware can actually sustain, and present it through a professional, scalable Windows Qt application suitable as the foundation of a future industrial HMI product.”

Optimize the complete pipeline:

Sensor
→ Capture
→ JPEG
→ Memory
→ Wi-Fi
→ Transport
→ Receive
→ Decode
→ Render
→ Display

The bottleneck may exist anywhere in this chain.

Measure before optimizing.

Benchmark before choosing architecture.

Verify before claiming.

Prefer the simplest architecture that achieves the required measured performance.

When tradeoffs are unavoidable, follow this priority:

Image Quality
>
Sustained FPS
>
Latency

while maintaining a usable real-time live-view experience.