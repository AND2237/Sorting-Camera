#include "quality_guard.h"

static bool at_least_percent(uint32_t bytes, uint32_t budget, uint32_t pct)
{
    return (uint64_t)bytes * 100u >= (uint64_t)budget * pct;
}

void quality_guard_init(quality_guard_t *g, int requested, int floor_q, int cap,
                        uint32_t budget_bytes)
{
    int base = requested < floor_q ? floor_q : requested;
    if (base < 0) {
        base = 0;
    }
    if (base > QG_QUALITY_MAX) {
        base = QG_QUALITY_MAX;
    }
    if (cap > QG_QUALITY_MAX) {
        cap = QG_QUALITY_MAX;
    }
    if (cap < base) {
        cap = base;
    }
    g->budget_bytes = budget_bytes;
    g->base = base;
    g->cap = cap;
    g->effective = base;
    g->have_raise = false;
    g->have_stepback = false;
    g->low_active = false;
    g->last_raise_us = 0;
    g->last_stepback_us = 0;
    g->low_since_us = 0;
    g->raise_events = 0;
    g->stepback_events = 0;
}

int quality_guard_observe(quality_guard_t *g, uint32_t frame_bytes, int64_t now_us)
{
    if (g->budget_bytes == 0) {
        return g->effective;
    }

    if (at_least_percent(frame_bytes, g->budget_bytes, QG_RAISE_PERCENT)) {
        g->low_active = false;
        if (g->effective < g->cap
            && (!g->have_raise || now_us - g->last_raise_us >= QG_RAISE_COOLDOWN_US)) {
            g->effective += QG_RAISE_STEP;
            if (g->effective > g->cap) {
                g->effective = g->cap;
            }
            g->last_raise_us = now_us;
            g->have_raise = true;
            g->raise_events++;
        }
    } else if (!at_least_percent(frame_bytes, g->budget_bytes, QG_LOW_PERCENT)) {
        if (!g->low_active) {
            g->low_active = true;
            g->low_since_us = now_us;
        } else if (g->effective > g->base
                   && now_us - g->low_since_us >= QG_LOW_HOLD_US
                   && (!g->have_stepback
                       || now_us - g->last_stepback_us >= QG_STEPBACK_PERIOD_US)) {
            g->effective -= 1;
            g->last_stepback_us = now_us;
            g->have_stepback = true;
            g->stepback_events++;
        }
    } else {
        g->low_active = false;
    }
    return g->effective;
}

/*
 * See quality_guard.h for the contract. Preconditions: quality_guard_observe()
 * has just produced a real transition (attempted != before) and the Sensor
 * refused to apply it. Undo exactly that one transition and nothing else - the
 * timers and hysteresis flags keep the state observe() gave them.
 */
void quality_guard_sensor_refused(quality_guard_t *g, int before, int attempted)
{
    if (attempted > before) {
        g->raise_events--;
    } else {
        g->stepback_events--;
    }
    g->effective = before;
}
