# ADR-0019: A device with no saved sensor record starts from the documented defaults

_Status: diagnosis confirmed on hardware 2026-10-11; fixed firmware not yet flashed._

## Context

Camera-on runs of the network bench (2026-10-09, `docs/benchmark-results.md`) reported the
same frame size, 37.2 KiB p50, for XGA q24, SXGA q24, UXGA q36 and UXGA q16, in three runs on
two firmwares. A verification run on 2026-10-10 saved one frame per profile and read the real
pixel size from each JPEG header:

| Requested | Real frame size | End marker |
|---|---|---|
| SVGA q12 | 800x600 | 100% |
| XGA q24 | **1600x1200** | 100% |
| SXGA q24 | **1600x1200** | 100% |
| UXGA q36 | 1600x1200 | 100% |
| UXGA q16 | 1600x1200 | 100% |

All five saved frames are completely black. A black JPEG is almost constant in size whatever
the quality (about 10 KiB at 800x600, 37 KiB at 1600x1200), which is what was measured. So:
the 37.2 KiB figures describe black frames, the fps and Mbps of the bench runs describe
trivial frames, and two resolutions were not produced at all.

## Cause

`camera_control_init()` (camera_control.c) did `memset(stored, 0, ...)` and then `load_nvs()`.
With no valid saved record (a new or erased chip, or a changed record version) `load_nvs()`
returned without touching `stored`, so every control read 0. The range check that is meant to
fall back to the default only triggers for an out-of-range value, and 0 is in range for most
controls. Reading the real defaults table in the same file and applying the old logic, 11 of 24
controls come up wrong when nothing is saved:

| Control | Value the old init applied | Documented default |
|---|---|---|
| aec (auto exposure) | 0 | 1 |
| aec_value | 0 | 300 |
| aec2 | 0 | 1 |
| agc (auto gain) | 0 | 1 |
| awb, awb_gain | 0 | 1 |
| bpc, wpc, raw_gma, lenc | 0 | 1 |
| dcw | 0 | 1 |

Auto exposure and auto gain off with a manual exposure of 0 gives a black picture. `dcw` is the
OV2640 DSP downscale enable: `set_dcw_dsp()` in esp32-camera writes bit 5 (DCW_EN) of CTRL2. With
it off, the modes that are scaled down from the 1600x1200 sensor window (XGA, SXGA, HD) would be
expected to leave the DSP at full size, while SVGA (native binned mode) and UXGA (native) are
unaffected. That matches the table above. The mechanism for dcw is read from driver source and
still to be confirmed on hardware (step 3 below).

The zeros were then saved by `save_nvs()` as if the user had chosen them, so the broken state
survived reboots and reflashes until something wrote different values (which is how the black
picture was fixed before, by changing gain settings in the Qt application).

## Hardware verification (2026-10-11)

`python benchmarks/sensor_tool.py --info` on the unflashed device confirmed the
prediction above: every control that normally defaults to on or non-zero read 0:

aec, aec2, aec_value, agc, awb, awb_gain, bpc, wpc, raw_gma, lenc, dcw.

`--restore-defaults` changed (current -> default): aec 0->1, aec2 0->1,
aec_value 0->300, agc 0->1, awb 0->1, awb_gain 0->1, bpc 0->1, wpc 0->1,
raw_gma 0->1, lenc 0->1, dcw 0->1. The next snapshot showed a visible scene:
snapshot 1280x720, brightness 132.9 / 255.

Re-run of the verification profiles on the restored device
(`benchmarks/results/phase2-cam-verify2-20261011-012921.json`, baseline firmware
NCB, one 10 s run per profile, all valid):

| profile | real size | frame KiB p50 | fps | link Mbps |
|---|---|---|---|---|
| svga q12 @18 | 800x600 | 16.4 | 9.75 | 1.38 |
| xga q24 @18 | 1024x768 | 20.3 | 8.25 | 1.45 |
| hd q24 @18 | 1280x720 | 22.6 | 6.22 | 1.18 |
| sxga q24 @18 | 1280x1024 | 32.8 | 6.00 | 1.67 |
| uxga q36 @18 | 1600x1200 | 43.6 | 4.25 | 1.43 |
| uxga q16 @18 | 1600x1200 | 55.1 | 2.50 | 1.20 |

XGA, HD, SXGA and UXGA report their requested sizes after the controls are
restored; dcw was the cause of the 1600x1200 output.

## Decision

1. `camera_control_init()` starts `stored[]` from the defaults table, not zeros; `load_nvs()` now
   returns whether a valid record was found, and a missing record logs a warning.
2. A record with `aec == 0` and `aec_value == 0` (a black picture by definition, and exactly what
   the old init saved) is repaired once to the defaults, with a warning. Records that
   differ in either field are left alone, so a deliberate user setting is never overwritten.
3. `benchmarks/sensor_tool.py` reads the sensor controls, restores every control to the default
   the device documents (`--restore-defaults`), and checks a snapshot (size, markers, and
   brightness when Pillow is installed). It is the check to run before any measurement.

## Consequences

- The results of the Phase 2 camera-on runs hold for the bench firmware comparison (same scene,
  same black frames on every firmware) but not as measurements of real frames. They are to be
  repeated after the sensor is restored. In particular the frame sizes, the link rates and the
  point at which frame size starts to limit fps were all measured on trivial frames.
- Devices that already saved the all-zero record are repaired on the next boot by decision 2;
  devices with a different saved record keep it.
- The first start after this change on an erased device runs with auto exposure and gain on.

## Verification still to do (hardware)

1. `python benchmarks/sensor_tool.py --info` on the device before the fix: expect the zero values above.
2. Flash the fixed firmware; boot log or `--info` shows the defaults restored; `--snapshot` shows a visible scene.
3. Re-run the verification profiles (`phase2_cam.py ... --profiles svga:12,xga:24,hd:24,sxga:24,uxga:36,uxga:16`):
   every profile reports its requested size. If XGA/SXGA still report 1600x1200 with dcw on,
   the dcw explanation is wrong and the cause is elsewhere (camera.c / the driver).
4. Erase NVS (`idf.py erase-flash`, reflash) and confirm a new device streams a visible picture.

## Related

- ADR-0018 (the tuning block; its camera-on A/B was measured on black frames)
- `docs/benchmark-results.md` "Network bench, Phase 2 (camera on)"
- `benchmarks/sensor_tool.py`, `firmware/esp32_cam_stream/main/camera_control.c`
