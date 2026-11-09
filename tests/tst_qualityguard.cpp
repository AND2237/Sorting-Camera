// Rules of the firmware quality guard (firmware/esp32_cam_stream/main/
// quality_guard.c). The module is pure C with no ESP-IDF dependency, so the
// firmware source is compiled straight into this test: what is tested here is
// the exact file the firmware links.
#include <QtTest>

extern "C" {
#include "../firmware/esp32_cam_stream/main/quality_guard.c"
}

namespace {
constexpr uint32_t kBudget = 262144;
constexpr int64_t kSec = 1000000;
constexpr int64_t kMs = 1000;
}

class TestQualityGuard : public QObject
{
    Q_OBJECT

private slots:
    void initStartsAtRequestedOrFloor()
    {
        quality_guard_t g;
        quality_guard_init(&g, 2, 6, 40, kBudget);
        QCOMPARE(g.base, 6);
        QCOMPARE(g.effective, 6);
        quality_guard_init(&g, 12, 6, 40, kBudget);
        QCOMPARE(g.base, 12);
        QCOMPARE(g.effective, 12);
    }

    void capNeverBelowBase()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 8, kBudget);
        QCOMPARE(g.cap, 12);
        QCOMPARE(quality_guard_observe(&g, kBudget, 0), 12);
        QCOMPARE(g.raise_events, 0u);
    }

    void smallFramesChangeNothing()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);
        for (int i = 0; i < 100; i++) {
            QCOMPARE(quality_guard_observe(&g, 60000, i * 50 * kMs), 12);
        }
        QCOMPARE(g.raise_events, 0u);
        QCOMPARE(g.stepback_events, 0u);
    }

    void raisesExactlyAtHalfBudgetWithCooldown()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);
        QCOMPARE(quality_guard_observe(&g, kBudget / 2 - 1, 0), 12);
        QCOMPARE(quality_guard_observe(&g, kBudget / 2, 0), 14);
        QCOMPARE(g.raise_events, 1u);
        QCOMPARE(quality_guard_observe(&g, kBudget, 100 * kMs), 14);
        QCOMPARE(quality_guard_observe(&g, kBudget, 250 * kMs), 16);
        QCOMPARE(g.raise_events, 2u);
    }

    void raiseStopsAtCap()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 16, kBudget);
        for (int i = 0; i < 10; i++) {
            quality_guard_observe(&g, kBudget, i * kSec);
        }
        QCOMPARE(g.effective, 16);
        QCOMPARE(g.raise_events, 2u);

        quality_guard_init(&g, 12, 6, 15, kBudget);  // an odd cap clamps the step
        for (int i = 0; i < 10; i++) {
            quality_guard_observe(&g, kBudget, i * kSec);
        }
        QCOMPARE(g.effective, 15);
        QCOMPARE(g.raise_events, 2u);
    }

    void stepsBackOneAtATimeNeverBelowBase()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);
        QCOMPARE(quality_guard_observe(&g, kBudget, 0), 14);
        QCOMPARE(quality_guard_observe(&g, 1000, 1 * kSec), 14);
        QCOMPARE(quality_guard_observe(&g, 1000, 11 * kSec - 1), 14);
        QCOMPARE(quality_guard_observe(&g, 1000, 11 * kSec), 13);
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec - 1), 13);
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec), 12);
        QCOMPARE(quality_guard_observe(&g, 1000, 60 * kSec), 12);
        QCOMPARE(g.stepback_events, 2u);
    }

    void midBandFrameRestartsTheLowTimer()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);
        quality_guard_observe(&g, kBudget, 0);                 // 14
        quality_guard_observe(&g, 1000, 1 * kSec);             // low starts
        quality_guard_observe(&g, kBudget * 40 / 100, 5 * kSec);  // 40%: not low
        quality_guard_observe(&g, 1000, 6 * kSec);             // low restarts
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec - 1), 14);
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec), 13);
    }

    void zeroBudgetDisablesTheGuard()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, 0);
        QCOMPARE(quality_guard_observe(&g, 0xFFFFFFFFu, 0), 12);
        QCOMPARE(g.raise_events, 0u);
    }

    void hugeFrameDoesNotOverflowTheComparison()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, 0xFFFFFFFFu);
        QCOMPARE(quality_guard_observe(&g, 0xFFFFFFFFu, 0), 14);
        quality_guard_init(&g, 12, 6, 40, kBudget);
        QCOMPARE(quality_guard_observe(&g, 0xFFFFFFFFu, 0), 14);
    }

    // The four tests below pin behaviour the existing nine could not reach: the
    // exact 35% band cutoff, the init clamps, and the Sensor-refusal rollback.
    // Refusal is exercised through quality_guard_sensor_refused(), the same
    // helper camera_guard_step() calls when a sensor refuses a quality change.
    // No real Sensor, register write, sensor call count, gate or driver state
    // is involved here; those stay source-inspection and hardware items.

    void lowBandBoundaryAt35Percent()
    {
        // Budget 262144 B: 35% lands at 91750.4 B, so 91750 B is inside the low
        // band and 91751 B is outside it. The difference shows in behaviour -
        // inside the band the low timer can drive a stepback, outside it the
        // guard never even arms.
        quality_guard_t low;
        quality_guard_init(&low, 12, 6, 40, kBudget);
        QCOMPARE(quality_guard_observe(&low, kBudget, 0), 14);  // steppable state
        QCOMPARE(quality_guard_observe(&low, 91750, 1 * kSec), 14);  // low: arms the hold
        QCOMPARE(quality_guard_observe(&low, 91750, 11 * kSec), 13);  // hold elapsed at 10 s
        QCOMPARE(low.stepback_events, 1u);

        quality_guard_t mid;
        quality_guard_init(&mid, 12, 6, 40, kBudget);
        QCOMPARE(quality_guard_observe(&mid, kBudget, 0), 14);
        QCOMPARE(quality_guard_observe(&mid, 91751, 1 * kSec), 14);  // not low: nothing arms
        QCOMPARE(quality_guard_observe(&mid, 91751, 11 * kSec), 14); // so no stepback, ever
        QCOMPARE(quality_guard_observe(&mid, 91751, 16 * kSec), 14);
        QCOMPARE(mid.stepback_events, 0u);
    }

    void initClampsRequestedAndCap()
    {
        quality_guard_t g;

        quality_guard_init(&g, 5, 10, 40, kBudget);  // requested below the floor
        QCOMPARE(g.base, 10);
        QCOMPARE(g.effective, 10);

        quality_guard_init(&g, 63, 0, 40, kBudget);  // requested at the maximum
        QCOMPARE(g.base, 63);
        QCOMPARE(g.effective, 63);
        // the cap may not sit below base, so it is pulled up with quality 63
        QCOMPARE(g.cap, 63);

        quality_guard_init(&g, 12, 6, 70, kBudget);  // cap above the maximum
        QCOMPARE(g.base, 12);
        QCOMPARE(g.effective, 12);
        QCOMPARE(g.cap, QG_QUALITY_MAX);

        quality_guard_init(&g, -5, -10, 40, kBudget);  // requested below zero
        QCOMPARE(g.base, 0);
        QCOMPARE(g.effective, 0);
    }

    void refusedRaiseRollsBackAndKeepsTheCooldown()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);

        // At t=0 a half-budget frame sits exactly on the raise threshold: the
        // guard proposes 14 and books the raise. The sensor then refuses it.
        const int attempted = quality_guard_observe(&g, kBudget / 2, 0);
        QCOMPARE(attempted, 14);
        QCOMPARE(g.raise_events, 1u);

        quality_guard_sensor_refused(&g, 12, attempted);
        QCOMPARE(g.effective, 12);
        QCOMPARE(g.raise_events, 0u);

        // The cooldown stays anchored at the refused attempt (t=0): a retry
        // 100 ms later proposes nothing and books nothing.
        QCOMPARE(g.last_raise_us, static_cast<int64_t>(0));
        QCOMPARE(quality_guard_observe(&g, kBudget / 2, 100 * kMs), 12);
        QCOMPARE(g.raise_events, 0u);

        // At exactly the 250 ms cooldown a further raise is allowed, once.
        QCOMPARE(quality_guard_observe(&g, kBudget / 2, 250 * kMs), 14);
        QCOMPARE(g.raise_events, 1u);
        QCOMPARE(g.effective, 14);
    }

    void refusedStepbackRollsBackAndKeepsThePeriod()
    {
        quality_guard_t g;
        quality_guard_init(&g, 12, 6, 40, kBudget);
        const int raised = quality_guard_observe(&g, kBudget, 0);  // 12 -> 14
        QCOMPARE(raised, 14);
        QCOMPARE(g.raise_events, 1u);

        // A 1000 B frame starts the 10 s low hold at t=1 s, so the first
        // allowed stepback lands at exactly t=11 s: 14 -> 13.
        QCOMPARE(quality_guard_observe(&g, 1000, 1 * kSec), 14);
        QCOMPARE(quality_guard_observe(&g, 1000, 11 * kSec), 13);
        QCOMPARE(g.stepback_events, 1u);

        quality_guard_sensor_refused(&g, 14, 13);
        QCOMPARE(g.effective, 14);
        QCOMPARE(g.stepback_events, 0u);

        // The stepback period stays anchored at the refused attempt (t=11 s):
        // a retry 1 us short of 5 s proposes nothing, a retry at exactly 5 s
        // steps back once.
        QCOMPARE(g.last_stepback_us, static_cast<int64_t>(11 * kSec));
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec - 1), 14);
        QCOMPARE(g.stepback_events, 0u);
        QCOMPARE(quality_guard_observe(&g, 1000, 16 * kSec), 13);
        QCOMPARE(g.stepback_events, 1u);
        QCOMPARE(g.effective, 13);
    }
};

QTEST_APPLESS_MAIN(TestQualityGuard)
#include "tst_qualityguard.moc"
