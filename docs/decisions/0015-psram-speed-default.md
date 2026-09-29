# ADR-0015: PSRAM clock is 40 MHz, and a defaults line cannot be trusted to set it

_Status: accepted 2026-09-29_

## Context

The Phase 6 audit raised **FW-3** as a P1 reproducibility defect and Task **A0** as the
second-highest-priority item:

> The PSRAM speed the benchmarks were measured at is not the speed the repository declares
> (FW-3). This is a reproducibility defect in the evidence chain itself…

Its evidence was textual: `sdkconfig.defaults` declares `CONFIG_SPIRAM_SPEED_80M=y` while
the local `sdkconfig` has `CONFIG_SPIRAM_SPEED_40M=y`, and `sdkconfig` is git-ignored. From
that the audit concluded that the measured build diverges from the committed configuration,
and proposed deciding between 40 and 80 MHz and re-measuring a point.

That reasoning reads a line as an intent. Kconfig does not work that way.

`esp_psram/esp32/Kconfig.spiram` declares:

```
config SPIRAM_SPEED_80M
    bool "Enable 80MHz clock"
    depends on ESPTOOLPY_FLASHFREQ_80M
```

`ESPTOOLPY_FLASHFREQ` is a separate symbol in `spi_flash/esp32/Kconfig.flash_freq`, defaulting
to `40M`. `sdkconfig.defaults` sets `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` — flash *capacity* —
and never sets flash *frequency*. The dependency is therefore unmet, `SPIRAM_SPEED_80M` is
never visible, and `kconfgen` discards the value.

It discards it **silently**. A clean configure run on 2026-09-29, with the `80M` line present,
printed no warning about it at all.

So the situation the audit described does not exist: there is no configuration to reconcile,
because the declared value never applied.

## Decision

1. **The PSRAM clock for this project is 40 MHz, stated explicitly.**
   `CONFIG_SPIRAM_SPEED_80M=y` is replaced by `CONFIG_SPIRAM_SPEED_40M=y`. It is the default
   anyway, but the point of a defaults file is to say what you actually run, and the previous
   line said the opposite of what the firmware ran at.

2. **A `sdkconfig.defaults` line is evidence of intent only; the generated `sdkconfig` is
   evidence of configuration.** When a claim about configuration matters, it is verified by
   generating one from a clean checkout, not by reading the defaults file. Reading is what
   produced this defect.

3. **Silent symbol discard is treated as a standing hazard, and commented at the site.**
   The comment names the dependency, the fact that it is silently dropped, and points here,
   so the next person cannot "restore" the line and believe the build got faster.

4. **No historical measurement is invalidated and no re-baseline is performed.** There is
   no configuration change to re-measure. See Evidence below — the generated configuration
   is byte-identical before and after this decision, which is a stronger statement than a
   single repeated frame-rate point could make, and needs no hardware to check.

5. **80 MHz PSRAM is a possible future lever, not a current setting.** Reaching it requires
   `ESPTOOLPY_FLASHFREQ_80M` as well, which changes frame-buffer bandwidth and therefore
   plausibly changes frame rate. It would be a new, unmeasured configuration requiring its
   own ADR and its own baseline, and it must never be adopted silently. It is of particular
   interest because Phase 5/6 left the HD profile at 8–11 fps against a 15 fps floor.

## Consequences

- The repository is reproducible from a clean checkout: `40 MHz` measured and `40 MHz`
  produced. No benchmark in the record needs relabelling or discarding.
- The audit's Task A0 is closed without hardware. The proposed "re-measure one HD/q12 point
  to confirm the choice does not move the frame rate" is unnecessary, because no choice
  moved — the diff proves it exhaustively rather than sampling it.
- Someone wanting the higher clock now has an explicit, warned, documented path instead of
  a line that looks like it already did the work.
- `sdkconfig` remains git-ignored (`.gitignore`), so the generated file still is not in the
  record. That is now acceptable rather than dangerous: it is reproducible, and the
  defaults file no longer lies about it. The alternative — committing `sdkconfig` — would
  freeze a file full of machine-local absolute paths and is not worth it.

## Evidence

Performed 2026-09-29, ESP-IDF v5.5.4, clean working copy.

| Step | Command | Result |
|---|---|---|
| Clean configure, **before** the fix | copy project without `sdkconfig`, `idf.py set-target esp32` | `CONFIG_ESPTOOLPY_FLASHFREQ="40m"`, `CONFIG_SPIRAM_SPEED_40M=y` |
| Measured configuration on this machine | `firmware/esp32_cam_stream/sdkconfig` | `CONFIG_ESPTOOLPY_FLASHFREQ="40m"`, `CONFIG_SPIRAM_SPEED_40M=y` |
| Diagnostic for the dropped line | delete `sdkconfig`, `idf.py reconfigure`, grep full output | **no warning emitted** |
| Clean configure, **after** the fix | same | same symbols |
| Full diff, before vs after | `Compare-Object` on both generated `sdkconfig` files | **IDENTICAL — no configuration change** |

The diff covers the whole generated file (72,127 bytes), not just the PSRAM symbols, so the
fix cannot have perturbed anything else in the configuration either.

Cross-checks that could have contradicted this and did not:

- No artifact in `benchmarks/results/` records a PSRAM or flash clock — the only `SPIRAM`
  occurrences are `free_spiram` telemetry, which is capacity and unaffected by clock. The
  artifact chain therefore neither supports nor contradicts a clock claim, and cannot be
  cited for one either way.
- `sdkconfig` has not been rewritten since 2026-09-27 14:42, while `build.ninja` was
  regenerated 2026-09-28 15:08 — a reconfigure that did not need to touch it, consistent
  with a stable configuration.

## Related

- **A0 / FW-3** — `docs/audits/GEMINI_PHASE6_AUDIT_2026-09-29.md` (frozen; its conclusion
  that the value was retained is correct, its reasoning and P1 severity are not)
- **ADR-0005** — ESP-IDF version pin; component updates are likewise deliberately frozen
- **INV-4** — XCLK 28/30 MHz investigation, the neighbouring unmeasured clock
- `docs/benchmark-results.md`, `docs/performance.md` — unchanged by this decision
