#pragma once

/*
 * Quality guard: a pure, host-testable rule set that raises the JPEG quality
 * *number* (smaller frames) when delivered frames approach the frame budget,
 * and walks it back when the scene calms down. No ESP-IDF includes, no clock,
 * no I/O: the caller passes the frame size and a monotonic time in
 * microseconds, and applies the returned number to the sensor itself.
 *
 * Limitation (by construction): the guard only sees frames the driver
 * delivered. A frame that overflows the budget is dropped inside the driver,
 * so the thresholds keep 2x headroom (raise at 50% of the budget) instead of
 * trying to react at 90%.
 */

#include <stdbool.h>
#include <stdint.h>

#define QG_QUALITY_MAX        63
#define QG_DEFAULT_CAP        40
#define QG_RAISE_STEP         2
#define QG_RAISE_PERCENT      50
#define QG_LOW_PERCENT        35
#define QG_RAISE_COOLDOWN_US  250000LL
#define QG_LOW_HOLD_US        10000000LL
#define QG_STEPBACK_PERIOD_US 5000000LL

typedef struct {
    uint32_t budget_bytes;   /* 0 disables the guard */
    int base;                /* lowest quality number allowed: max(requested, floor) */
    int cap;                 /* highest quality number allowed, >= base */
    int effective;           /* quality number to apply, base <= effective <= cap */
    bool have_raise;
    bool have_stepback;
    bool low_active;
    int64_t last_raise_us;
    int64_t last_stepback_us;
    int64_t low_since_us;
    uint32_t raise_events;
    uint32_t stepback_events;
} quality_guard_t;

void quality_guard_init(quality_guard_t *g, int requested, int floor_q, int cap,
                        uint32_t budget_bytes);

/* Feed one delivered frame. Returns the quality number to apply for the
 * following frames (the same value as g->effective after the update). */
int quality_guard_observe(quality_guard_t *g, uint32_t frame_bytes, int64_t now_us);

/*
 * Rollback for a Sensor that refused the number observe() just proposed: undo
 * exactly the event counter of that one transition and restore the quality
 * number the Sensor is actually running. Preconditions: quality_guard_observe()
 * returned a value different from `before`, and the Sensor operation that
 * applied it has failed. The cooldown and hysteresis state observe() set is
 * deliberately left standing - that is what keeps a refused change from
 * spinning. No Sensor I/O happens here; the caller keeps the Sensor calls.
 */
void quality_guard_sensor_refused(quality_guard_t *g, int before, int attempted);
