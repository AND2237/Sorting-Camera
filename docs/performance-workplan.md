# Performance workplan: best resolution x quality x fps on ESP32-CAM + OV2640

_Written 2026-10-05 from the firmware sources, the ESP-IDF v5.5.4 / esp32-camera v2.1.7 sources, and
this repository's own benchmark record. **Nothing in this file was run on hardware.** Every number is
tagged **[measured]** (read from `docs/benchmark-results.md` or `benchmarks/results/`),
**[derived]** (arithmetic on measured numbers) or **[hypothesis]** (to be confirmed by a step below).
Per `AGENTS.md`: every change below goes baseline -> change -> measured result -> ADR, and anything
that does not win on a real metric is reverted._

## 1. Goal and acceptance

Priority order (`AGENTS.md`): image quality > sustained fps (floor 15, preferred 20+) > latency.
Example target from the owner: 1600x1200 at 15 fps.

A profile is accepted only if, on the fixed test scene of phase 0, over a 20-minute run:

| Metric | Requirement |
|---|---|
| mean fps | at or above the target |
| p5 of per-second fps | at least 85% of target (no long dips) |
| capture failures, camera recoveries, device resets | 0 / 0 / 0 |
| frame bytes | p95 below 90% of `CAMERA_JPEG_MODE_FRAME_SIZE` (262,144 B), the frame budget |
| PC/HMI decode p95 | below half the frame interval |

## 2. What the existing record says

| # | Fact | Source |
|---|---|---|
| F1 | fps sits on fixed plateaus once frames are small enough: **11.25** (XGA, SXGA, HD at q24 and q36; XGA also at q12), **22.5** (SVGA, VGA), **45** (QVGA). The plateau values are exact halvings of each other. | Phase 4 envelope **[measured]** |
| F2 | On the 11.25 plateau the frame size ranges 22.8 KB (XGA q36) to 46.9 KB (SXGA q24), i.e. 2.0 to 4.2 Mbps, with the same fps. So the plateau is not a byte-rate cap. (The Phase 4 note that calls it a "Wi-Fi byte-rate cap" is contradicted by this, and by the later Phase 5 analysis.) | **[derived]** from Phase 4 tables |
| F3 | SVGA q36, XCLK 18 -> 27 MHz: 22.4 -> 33.6 fps (x1.50 for x1.50 clock); 20 MHz is a reproducible dip (18.6). | XCLK sweep **[measured]** |
| F4 | HD q12: XCLK 18 -> 7.7 fps, 20 -> 5.9, 22 -> 2.2 (frames inflate 58 KB -> 246 KB), 24+ -> no frames. HD is capped at 20 MHz in firmware. | XCLK sweep **[measured]** |
| F5 | XCLK ceilings for XGA, SXGA and UXGA were **never measured**; `camera_xclk_max_mhz()` returns 27 for them and every profile defaults to 18 MHz. | `camera.c`, `app.h` |
| F6 | In the large-frame regime the frame interval is linear in frame size at about 2.2 ms/KiB (HD, 18 MHz). UXGA data gives 1.85 to 2.1 ms/KiB for q12 to q36 (q4: 2.9). | Phase 5 **[measured]**; UXGA slope **[derived]** |
| F7 | UXGA at 18 MHz: 1.04 / 3.82 / 7.45 / 9.81 fps for q4 / q12 / q24 / q36, frames 344 / 130 / 71 / 56 KB. | Phase 4 envelope **[measured]** |
| F8 | Same config gives 7.7 to 11.3 fps on different days; frame size drifts up to 2x with the scene (UXGA q12: 128,659 B vs 259,471 B). | `camera.c` comment, Phase 5 **[measured]** |
| F9 | Peak link throughput ever observed: 4.36 Mbps (HTTP), 4.74 Mbps (UDP). The link was never driven harder than the camera could feed it. | Phase 3/5 **[measured]** |
| F10 | Camera task on core 0 is best (11.0 fps) vs core 1 (8.5) and no affinity (9.2). fb_count 3 beats 2 by 7 to 15%. | Phase 5 **[measured]** |

### Working model (a reading of F1 to F7, not a measured law) **[hypothesis]**

    frame period  =  max( sensor-mode readout time ,  ~2.0 ms/KiB x frame size )

- Sensor-mode readout time scales with 1/XCLK. For the UXGA-readout family (UXGA, SXGA, HD, XGA) it is
  89 ms at 18 MHz (F1). Predicted ceilings: **15.0 fps at 24 MHz, 16.9 fps at 27 MHz.** Measured only
  for SVGA (F3); HD breaks above 20 MHz (F4); the other three modes are untested (F5).
- The byte term gives 15 fps at roughly **30 to 35 KiB per frame**, and 9 to 10 fps at 56 KiB. Whether
  the slope itself changes with XCLK is unknown (step 3.3).

Consequence for the example target: UXGA at 15 fps needs both a clean XCLK of 24 MHz or more **and**
frames of about 33 KiB or less. UXGA q36 is already 56 KB (F7), so the quality needed for 33 KiB is
worse than anything measured. This is the model's prediction, to be tested in step 3.4, not a verdict.

Candidate profiles for 15 fps **[derived]**:

| Profile | Frame B (p50, Phase 4) | Link need at 15 fps | Needs | Status |
|---|---|---|---|---|
| SVGA q12 | 26,737 | 3.2 Mbps | nothing new | 19.65 fps at 18 MHz **[measured]** |
| XGA q24 | 26,746 | 3.2 Mbps | clean XCLK >= 24 MHz (>= 26 for margin) | untested |
| HD q36 | 27,761 | 3.3 Mbps | HD cannot exceed 20 MHz (ceiling about 12.5 fps) | cannot reach 15 |
| SXGA q36 | 38,463 | 4.6 Mbps | byte term gives about 13 fps | cannot reach 15 |
| UXGA q36 | 56,409 | 6.8 Mbps | byte term gives about 9 fps | cannot reach 15 |
| UXGA q24 | 71,173 | 8.5 Mbps | byte term gives about 7 fps | cannot reach 15 |

## 3. Defects and limits found in the current implementation

Ranked by expected effect on the goal. "Evidence" is a file or a record entry; nothing here has been
fixed yet except D5's logging.

**D1. Quality is fixed, frame size is not, so fps is not stable (F8).**
Evidence: `camera.c` (`s_quality`, only changed by a config call), Phase 5 "why 11-12 fps sometimes and
7-8 other times". A fixed `q` means the picture is as good as the scene allows and the fps is whatever
results. For a fps target the controlled variable should be bytes per frame, with quality as the
actuator. The no-reinit path already exists (`camera_apply_config`, `only_quality` branch, calls
`sensor->set_quality`). Fix: phase 5, step 5.2. Risk: oscillation (needs hysteresis), and never go
below `camera_quality_floor()`.

**D2. The fps ceiling of the high-resolution modes was never explored along its main axis, XCLK (F3, F4, F5).**
Evidence: `app.h` `CAM_DEFAULT_XCLK_HZ 18000000` for every mode; `camera_xclk_max_mhz()` only encodes
the HD result. Under the working model, XCLK is the lever that moves the 11.25 plateau. Fix: phase 3
step 3.1 measures it, step 5.1 stores one clean XCLK per resolution. Risk: noise inflation and a
no-frames state at too high a clock (F4); the existing recovery guard covers it.

**D3. Clean XCLK headroom is probably limited by signal or supply integrity, not by the sensor.**
Evidence: the failure signature at high XCLK is JPEG inflation (58 KB -> 246 KB, "noise"), and
brownout resets are recorded under heavy streaming. **[hypothesis]** A better 5 V supply and
decoupling may raise the clean XCLK. Fix: phase 4. Risk: none to the firmware; it is hardware work,
judged only by the "valid" criterion of step 3.1.

**D4. The link was never characterised (F9).** Fix: phase 1 and 2, `net_bench`. This decides whether the
ADR-0018 tuning block stays or is reverted.

**D5. Wi-Fi radio settings are library defaults and were never recorded.** `wifi.c` never calls
`esp_wifi_set_protocol`, `set_bandwidth` or `set_max_tx_power`; the Wi-Fi task and the camera task both
default to core 0 (`ESP_WIFI_TASK_PINNED_TO_CORE_0`, `CAMERA_CORE0`). The camera-task core was tested
(F10); the Wi-Fi task core was not. The firmware now logs protocol, bandwidth, channel and TX power
at boot (`wifi.c`, line "radio:"). Candidate fixes: phase 5, step 5.4.

**D6. Three `httpd_resp_send_chunk` calls per frame produce nine `send()` calls.** Each chunk is sent as
three separate `send()`s (size line, data, trailing CRLF; verified in `httpd_txrx.c`), so a frame
is three size lines, three CRLFs, the boundary, the part header and the JPEG; eight of the nine are under
about 100 bytes. Merging boundary and part header into one chunk removes one chunk (three `send()`
calls) per frame with an identical byte stream after de-chunking. Expected effect: small (CPU, latency),
not fps. Low priority, step 5.3.

**D7. Not reviewed: the Qt PC application and the HMI.** Decode and render cost at 1280x1024 and above
on the HMI hardware is unknown. Step 6 measures it. If the HMI cannot decode a profile in time, no
firmware work matters for that profile.

**Not defects (checked and ruled out by arithmetic):**
- PSRAM speed. The camera produces about 0.5 MB/s of JPEG (F6); PSRAM at 40 MHz moves on the order of
  10 MB/s (general knowledge, not measured here). `SPIRAM_SPEED_80M` remains an unneeded lever.
- `fb_count` and `grab_mode`: already measured, 3 / latest wins (F10).
- Camera task affinity: already measured (F10).
- Nagle: ESP-IDF httpd sets `TCP_NODELAY` around each chunk (ADR-0010).

## 4. Decision tree

    PHASE 1-2  link test (net_bench)
        |
        +-- int-mode rate  <  2 x link need of the target profile   -> BRANCH L (link-limited): phase 5.4, then re-test
        |
        +-- int-mode rate  >= 2 x link need                          -> link is not the limit
                |                                                       ADR-0018 block: keep only if it won in the cam-mode A/B
                v
           PHASE 3  attribute the fps ceiling at the target mode
                |
                +-- XCLK raises fps and output stays valid           -> BRANCH C1: store per-mode XCLK (5.1)
                +-- XCLK raises noise / zero frames first            -> BRANCH C2: phase 4 (hardware), then 3.1 again
                +-- fps follows frame bytes, not XCLK                -> BRANCH C3: quality controller (5.2) + smaller profile
                |
                v
           PHASE 5  firmware changes, one at a time, A/B each
           PHASE 6  PC / HMI check        PHASE 7  acceptance + docs

"Link need" of a profile = frame bytes x target fps x 8. Required margin: at least 2x, because Wi-Fi
rate varies with interference and the per-second p5 matters more than the mean.

## 5. Steps

### Phase 0. Preparation (one hour, once)

0.1 Work in the working tree; commit only when asked (`AGENTS.md`). Keep `sdkconfig` out of git (it is
ignored); each variant below uses its own build directory and its own `sdkconfig`.

0.2 **Fixed test scene.** A printed page of small text plus a photo, on a stand at a fixed distance,
lit by one fixed lamp (not daylight; F8 shows scene content moves fps by tens of percent). Do not
move it for the whole campaign. Record the lamp and distance in the result notes.

0.3 **Power.** Feed the ESP32-CAM 5 V through its 5 V pin from a supply rated 2 A, with the shortest
cable you have, not from a laptop USB port. Add 470 uF across 5 V/GND at the board. Read the serial
console for `Brownout detector` lines; any such line invalidates the run.

0.4 **Wi-Fi environment.** Record the PC adapter (the record says Intel AX201), the distance (keep it
under 1 m, line of sight), and the other 2.4 GHz networks visible on channels 1, 6 and 11.

0.5 Flash nothing yet. Print `docs/benchmark-results.md` headings you will append to.

### Phase 1. Build and flash the four firmware variants

All commands from `firmware/esp32_cam_stream`, after `scripts\idf-env.ps1`. Each variant has its own
build directory and its own generated `sdkconfig`, so one never inherits another's values (ADR-0015).

    # P  = production, new block (the ADR-0018 trial)
    idf.py -B build-p -D SDKCONFIG=build-p/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32
    idf.py -B build-p build

    # PB = production, baseline settings (the A side of every A/B)
    idf.py -B build-pb -D SDKCONFIG=build-pb/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults" set-target esp32
    idf.py -B build-pb build

    # NB = net bench without camera, new block;     NBB = same with baseline settings
    idf.py -B build-nb  -D SDKCONFIG=build-nb/sdkconfig  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench-nocam.defaults"          set-target esp32
    idf.py -B build-nbb -D SDKCONFIG=build-nbb/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults;sdkconfig.bench-nocam.defaults" set-target esp32

    # NC = net bench WITH camera, new block;        NCB = same with baseline settings
    idf.py -B build-nc  -D SDKCONFIG=build-nc/sdkconfig  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench-cam.defaults"          set-target esp32
    idf.py -B build-ncb -D SDKCONFIG=build-ncb/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.baseline.defaults;sdkconfig.bench-cam.defaults" set-target esp32

(then `idf.py -B <dir> build` for each; flash with `idf.py -B <dir> -p COMPx flash monitor`.)

1.1 **Verify what you built, from the generated file, never from the defaults file:**

    findstr /R "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ= CONFIG_ESPTOOLPY_FLASHFREQ= CONFIG_LWIP_TCP_WND_DEFAULT= CONFIG_LWIP_IRAM_OPTIMIZATION CONFIG_SORTING_CAM" build-nb\sdkconfig

Expected: new = `240`, `"80m"`, `32768`, `=y`; baseline = `160`, `"40m"`, `5760`, not set. The bench
firmware also prints its own `build cfg:` line on the serial console when a client connects; copy it
into the result record.

1.2 Flash NBB first. On the serial console confirm `network bench build: camera NOT initialised`,
`bench server listening on tcp/83` and the `radio:` line (record protocol, bandwidth, channel, power).

The bench builds do not start the MJPEG server or the frame transport (socket budget and a clean
link). They keep the control server on port 80, so `/api/v1/status` still works.

### Phase 2. Link test (answers "is the link the limit?")

Join the camera's Wi-Fi. For each firmware (NBB then NB, no camera) run the matrix below. Each cell
is 30 s, three repetitions, `--warmup 3`, the PC doing nothing else.

    python benchmarks/net_bench.py --mode int   --chunk 8192  --seconds 30 --warmup 3 --label baseline
    python benchmarks/net_bench.py --mode int   --chunk 1460  --seconds 30 --warmup 3 --label baseline
    python benchmarks/net_bench.py --mode int   --chunk 32768 --seconds 30 --warmup 3 --label baseline
    python benchmarks/net_bench.py --mode psram --chunk 8192  --seconds 30 --warmup 3 --label baseline

Then flash NCB and NC (camera running) and run `--mode cam --seconds 60` for each, once at the profile
under test (set it through the normal config API first) and once at SVGA q12 as a control.

Record, per cell: PC `mean_mbps_whole_run`, per-second p5/p50/p95, `stalled_seconds`, and from the
serial console the device's `device_mbps`, `slow_sends`, `max_send_ms`, `min_free_internal`.
The PC and device Mbps must agree within a few percent; if not, the run is invalid.

Read the result like this:

| Observation | Meaning | Action |
|---|---|---|
| `int` p50 >= 2 x link need of the target profile, baseline and new alike | Link is not the limit. | Go to phase 3. ADR-0018 block unproven; keep it only if `cam` mode improves. |
| `int` new clearly above `int` baseline (more than the run-to-run spread) | The tuning block works at the link level. | Keep; then check `cam` mode before crediting it with fps. |
| `int` new equals `int` baseline | The block does nothing for throughput. | Revert the block (ADR-0018 rule). |
| `psram` far below `int` | Reading frames from external RAM costs throughput. | Consider copying only if `cam` shows the same gap; PSRAM 80 MHz stays a separate ADR-0015 lever. |
| `cam` well below `psram` | Camera DMA/capture interferes with sending. | Phase 5.4 (core placement); phase 3. |
| `slow_sends` > 0 or `max_send_ms` > 200 | TCP window stalls (the PC or the air, not the camera). | Check channel, distance, PC adapter power saving. |
| `int` p5 < 0.5 x p50 | Rate is unstable; the mean hides it. | Phase 5.4 channel/bandwidth tests before any camera work. |

**Gate G1.** Write the table into `docs/benchmark-results.md` (new section "Network bench"), with the
firmware git hash, the `build cfg:` and `radio:` lines, the PC adapter, distance, date and duration.
Update ADR-0018's status to "kept" or "reverted" with the numbers.

### Phase 3. Attribute the fps ceiling at the target modes

Flash P (new block; or PB if G1 reverted it). Use `benchmarks/phase5_pipeline.py` exactly as in the
record (`--framesize`, `--quality`, `--xclk`, `--seconds 60`), fixed scene, firmware untouched.

3.1 **XCLK sweep in the UXGA-readout family, never measured (F5).** For each of
`xga q24`, `sxga q24`, `uxga q24`: XCLK 18, 20, 22, 24, 26, 27 MHz. One run per cell first;
repeat the two best and the first failing cell three times.
A cell is **valid** only if: recoveries 0, capture failures 0, frame bytes p50 within +-15% of the
same mode at 18 MHz (inflation is the noise signature, F4), no `Brownout` on the console.
Stop raising the clock for a mode at its first invalid cell; do not run higher cells of that mode
(the HD record shows recovery storms and a stuck sensor).
Compare the fps against the model's prediction `11.25 x XCLK / 18` and note the 20 MHz dip.

3.2 **Plateau check.** At each valid cell, fps within 3% of the model => sensor-readout-limited; fps
clearly below the model while frame bytes are small => something else (exposure in low light, or the
send path). Rule out exposure by repeating one cell with `aec=0` and a short manual `aec_value`
(the controls exist in `camera_control.c`); fps must not change in a bright scene.

3.3 **Does the byte slope move with XCLK?** At the best valid XCLK of one mode, run q12, q24, q36, q48.
Fit frame interval against frame KiB. If the slope stays near 2 ms/KiB, the byte budget for a target
fps is `1000 / (fps x slope)` KiB (about 33 KiB at 15 fps). If it drops with XCLK, the budget grows
by the same ratio.

3.4 **UXGA quality reality check.** At the best valid UXGA clock, run q36, q44, q52, q63 and save the
snapshot (`/api/v1/snapshot`) of the fixed scene at each. Decide by eye, with the printed page, whether
the quality that reaches 15 fps is acceptable. This is the step that settles the 1600x1200 question.

**Gate G2.** Fill the table "Mode / best valid XCLK / fps / frame bytes / branch (C1, C2 or C3)" in
`docs/benchmark-results.md`.

### Phase 4. Hardware integrity (only if G2 says C2: noise appears before the sensor limit)

Change one thing at a time and re-run only the first failing cell of step 3.1; success is that cell
turning valid.

4.1 Power: separate 5 V supply with a 470 uF + 100 nF at the 5 V pin; measure the 3.3 V rail under
streaming with a multimeter or scope if you have one (a dip below about 3.0 V explains inflation and
brownouts).
4.2 Seating: reseat the camera ribbon cable and clean the contacts.
4.3 Heat: let the board warm to steady state before measuring; note whether fps drifts.
4.4 Different board or module of the same type, to separate a bad unit from the design.
Stop when a cell turns valid; record what fixed it. If nothing does, accept the lower XCLK as the
mode's ceiling.

### Phase 5. Firmware changes (one at a time; each: baseline run, change, run, ADR, keep or revert)

**5.1 Per-mode XCLK (branch C1).** Replace `camera_xclk_max_mhz()`'s single HD case with the measured
table of step 3.1 (keep a 2 MHz margin below the first invalid cell), and make the profile (app side:
`ProfileEngine`, not reviewed) send that clock with its resolution. Expected: the 11.25 plateau moves
up by the clock ratio, if the model holds. Risk: a clock that is valid in one scene but not another; the
20-minute soak of phase 7 is the test.

**5.2 Byte-budget quality controller (branch C3, and the answer to D1).** Where: the stream loop in
`http_servers.c` already sees every `fb->len`. Design:
- Input: exponential moving average of frame bytes over about 15 frames.
- Setpoint: `budget_B = 1024 x 1000 / (target_fps x slope)` from step 3.3, never above 90% of the
  262,144 B frame budget.
- Actuator: `sensor->set_quality()` through `camera_lock()`, +-1 per decision, at most one decision per
  second, a dead band of +-8% around the setpoint (hysteresis), clamped between
  `camera_quality_floor(fs)` and a user ceiling.
- Rule: quality is lowered when the average exceeds the setpoint, raised when it is below 80% of it.
- Expose the current quality and the setpoint in `/api/v1/status` so the benchmark can see them.
- Acceptance: fps standard deviation over the soak at least 3x lower than fixed quality on the same
  scene, mean quality within 2 steps of the best fixed quality that holds the target.
Risk: a `set_quality` change takes effect on a later frame (the driver pipeline holds up to 3), so the
control loop must not react faster than that; never allow the controller to leave the measured floor.

**5.3 Send-path coalescing (D6).** In `stream_handler`, build `boundary + part header` into one buffer
and send it as one chunk (two chunks per frame instead of three). Same bytes after de-chunking; the Qt
capture/decoder tests must be re-run against it. Measure CPU and `frame_age` p95, not fps.
Keep only if latency or CPU improves beyond the run spread.

**5.4 Wi-Fi tests, each alone, measured with `net_bench --mode cam` and the real stream:**
a. `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1` (new variable; the camera task stays on core 0);
b. protocol 11g+11n only (drop 11b): `esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)`;
c. bandwidth HT20 vs HT40, set explicitly, with the `radio:` line proving the value;
d. channel 1 vs 6 vs 11, picked from the step 0.4 survey;
e. `CONFIG_LWIP_TCPIP_TASK_PRIO` raised (the iperf example uses 23), only if (a) to (d) showed a link effect.
Each is kept only if it beats the spread in both `int`/`cam` modes. Do not stack them before each has
a verdict.

**5.5 Research branch, only if G2 shows the sensor limit is below what the clock should give.** The
OV2640 frame time is set by registers the driver writes itself (`CLKRC` in the sensor bank and
`R_DVP_SP`, 0xD3, in the DSP bank; `set_window()` in `sensors/ov2640.c`), and this repository never
reads or changes them. Read both with `sensor->get_reg` at each mode and log them. Changing them is
unverified territory: one register, one value, one run, with the recovery guard active, and never in
the production build. I have not verified what any value does on this board.

**5.6 If G2 and 3.4 say the target is unreachable on this hardware.** The honest options are a lower
profile with the controller of 5.2, or different hardware. The `esp32-camera` driver already supports
ESP32-S3 (camera peripheral with PSRAM DMA, `CAMERA_PSRAM_DMA` in its Kconfig) and OV5640; that
route is untested here and would be its own project.

### Phase 6. PC and HMI check

6.1 On the PC, `phase5_pipeline.py` already records decode, parse and render times and CPU; confirm
decode p95 is under half the frame interval at the chosen profile.
6.2 **On the HMI hardware itself** (not the PC): run the Qt app against the camera at the chosen
profile for 10 minutes; record CPU, decoded fps, dropped frames. Software JPEG decode of large frames
on a small embedded CPU is a likely limit; if it fails, lower the profile for the HMI, not the camera.
6.3 Frames: latest-frame-wins, bounded buffers, every drop counted (`AGENTS.md`); verify the counters
show zero silent drops at the chosen profile.

### Phase 7. Acceptance and records

7.1 Soak each candidate final profile for 20 minutes, three times on different days with the same lamp
and scene, against section 1.
7.2 Append to `docs/benchmark-results.md`: firmware hash, `build cfg:`, `radio:`, config, PC and HMI,
date, duration, raw JSON path under `benchmarks/results/`.
7.3 One ADR per kept change (0019 and up); one line in `docs/decisions` index; update README counts.
7.4 Revert anything that did not win, with the numbers that say so.

## 6. What this plan does not know

- Whether the link is a limit at all (phase 2 decides).
- Whether the UXGA-family clock can be raised cleanly on this board (F5).
- What the HMI hardware is and how fast it decodes JPEG (not reviewed; step 6.2).
- The effect of any OV2640 register change (not verified; step 5.5 is labelled research).
- The Qt `ProfileEngine` and the HMI app were not read for this plan.
- Nothing in the firmware changes of this campaign has been flashed; all firmware variants were built
  and linked, not run.
