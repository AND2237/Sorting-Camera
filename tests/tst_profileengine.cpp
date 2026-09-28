// Section 12 asks for profiles that optimise by image quality > FPS > latency,
// and says the useful operating points have to come from experiment rather than
// assumption. So this test checks two separate things: that the ladder is
// ordered by the project's own priority, and that Automatic's answer follows
// from the data instead of being hard-coded to a profile id.
#include "ProfileEngine.h"

#include <QSignalSpy>
#include <QTest>

using Profile = ProfileEngine::Profile;
using Id = ProfileEngine::ProfileId;

class TestProfileEngine : public QObject
{
    Q_OBJECT

private slots:
    void ladderIsOrderedByImageQualityFirst();
    void everyLadderEntryIsAMeasuredOperatingPoint();
    void floorsAreOnlyLoweredByARecordedDecision();
    void recommendedIsTheLargestPointClearingItsFloor();
    void matchConfigRecognisesEachProfile();
    void matchConfigReportsAnOffLadderConfigurationAsCustom();
    void resolutionAndGrabModeVocabularyIsTranslated();
    void setConfigIdentifiesTheActiveProfile();
    void changingConfigurationResetsTheShortfallCount();
    void aSingleSlowWindowDoesNotProduceAdvice();
    void sustainedShortfallProducesAdviceNamingTheNextRung();
    void aRecoveredFrameRateClearsTheShortfall();
    void manualModeNeverAdvises();
    void theBottomRungHasNothingToOffer();
    void closestByResolutionFindsTheNearestMeasuredPoint();
};

void TestProfileEngine::ladderIsOrderedByImageQualityFirst()
{
    const QList<Profile> &l = ProfileEngine::ladder();
    QVERIFY(l.size() >= 4);

    // Quality first means descending pixel count. Ties break on the higher
    // JPEG quality, which is the same ordering applied one step finer.
    for (int i = 1; i < l.size(); ++i) {
        const bool ordered = l.at(i).pixels < l.at(i - 1).pixels
            || (l.at(i).pixels == l.at(i - 1).pixels && l.at(i).quality > l.at(i - 1).quality);
        QVERIFY2(ordered, qPrintable(QStringLiteral("%1 does not rank below %2")
                                         .arg(l.at(i).name, l.at(i - 1).name)));
    }
}

void TestProfileEngine::everyLadderEntryIsAMeasuredOperatingPoint()
{
    const QList<Profile> &l = ProfileEngine::ladder();
    for (const Profile &p : l) {
        QVERIFY2(p.measuredFps > 0.0, qPrintable(p.name + " carries no measurement"));
        QVERIFY2(!p.framesize.isEmpty(), qPrintable(p.name + " names no resolution"));
        QVERIFY2(!p.evidence.isEmpty(),
                 qPrintable(p.name + " cites no run, so the number is unexplained"));
        QVERIFY2(p.pixels > 0.0, qPrintable(p.name + " has no pixel count"));
        // Everything in the ladder uses the machine settings ADR-0010 settled
        // on; a rung that quietly changed the clock would not be comparable
        // with the measurements it cites.
        QCOMPARE(p.xclkMhz, 18);
        QCOMPARE(p.frameBufferCount, 3);
        QCOMPARE(p.grabMode, QStringLiteral("latest"));

        // The flag has to agree with the measurement and the floor, or the UI
        // would label a profile as clearing its floor when the numbers say
        // otherwise.
        QCOMPARE(p.meetsFloor, p.measuredFps >= p.floorFps);
    }
}

void TestProfileEngine::floorsAreOnlyLoweredByARecordedDecision()
{
    // Section 34's general floor is 15 fps. Only HD was scoped below it, by
    // the owner, in ADR-0010. Any other rung below 15 is a mistake rather than
    // a decision, and lowering a floor is exactly the way to make an unusable
    // profile selectable again.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (p.floorFps < 15.0) {
            QCOMPARE(p.id, Id::HighQuality);
        }
    }
    const Profile *hq = ProfileEngine::byId(Id::HighQuality);
    QVERIFY(hq);
    QCOMPARE(hq->floorFps, 7.0);
    QVERIFY(hq->measuredFps >= hq->floorFps);

    // Maximum Quality is offered, and is honestly under the floor.
    const Profile *mq = ProfileEngine::byId(Id::MaximumQuality);
    QVERIFY(mq);
    QVERIFY(mq->measuredFps < mq->floorFps);
    QVERIFY(!mq->meetsFloor);
}

void TestProfileEngine::recommendedIsTheLargestPointClearingItsFloor()
{
    const Profile *rec = ProfileEngine::recommended();
    QVERIFY(rec);

    // The rule, applied to the table rather than asserted against a name: no
    // profile that clears its own floor may have more pixels than the
    // recommendation.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (p.meetsFloor && p.measuredFps >= p.floorFps) {
            QVERIFY2(rec->pixels >= p.pixels,
                     qPrintable(QStringLiteral("recommended %1, but %2 clears its own floor and "
                                               "has more pixels")
                                    .arg(rec->name, p.name)));
        }
    }

    // Sanity on the outcome, not on the mechanism: with the measured envelope
    // this resolves to HD q12, the production default ADR-0010 chose. If the
    // data changes, this assertion is what should be revisited - not the rule.
    QCOMPARE(rec->id, Id::HighQuality);
}

void TestProfileEngine::matchConfigRecognisesEachProfile()
{
    for (const Profile &p : ProfileEngine::ladder()) {
        QCOMPARE(ProfileEngine::matchConfig(p.framesize, p.quality, p.xclkMhz,
                                            p.frameBufferCount, p.grabMode),
                 p.id);
    }
}

void TestProfileEngine::matchConfigReportsAnOffLadderConfigurationAsCustom()
{
    // One field away from High Quality in three different ways. Custom is a
    // legitimate answer: the user is entitled to an operating point the ladder
    // does not describe, and the app must not pretend otherwise.
    QCOMPARE(ProfileEngine::matchConfig(QStringLiteral("hd"), 10, 18, 3, QStringLiteral("latest")),
             Id::Custom);
    QCOMPARE(ProfileEngine::matchConfig(QStringLiteral("hd"), 12, 20, 3, QStringLiteral("latest")),
             Id::Custom);
    QCOMPARE(ProfileEngine::matchConfig(QStringLiteral("hd"), 12, 18, 2, QStringLiteral("latest")),
             Id::Custom);
    QCOMPARE(ProfileEngine::matchConfig(QStringLiteral("vga"), 24, 18, 3, QStringLiteral("cont")),
             Id::Custom);
    QCOMPARE(ProfileEngine::matchConfig(QStringLiteral("hd"), 12, 18, 3, QStringLiteral("cont")),
             Id::Custom);
}

void TestProfileEngine::resolutionAndGrabModeVocabularyIsTranslated()
{
    QCOMPARE(ProfileEngine::framesizeKeyFromResolution(QStringLiteral("1280x720")),
             QStringLiteral("hd"));
    QCOMPARE(ProfileEngine::framesizeKeyFromResolution(QStringLiteral("1600x1200")),
             QStringLiteral("uxga"));
    QCOMPARE(ProfileEngine::framesizeKeyFromResolution(QStringLiteral("nonsense")), QString());
    QCOMPARE(ProfileEngine::framesizeKeyFromResolution(QString()), QString());

    // The firmware calls it when_empty, the ladder calls it cont. If this
    // translation were missing, a camera running in the non-default grab mode
    // would be reported as Custom for no visible reason.
    QCOMPARE(ProfileEngine::grabModeFromFirmware(QStringLiteral("when_empty")),
             QStringLiteral("cont"));
    QCOMPARE(ProfileEngine::grabModeFromFirmware(QStringLiteral("latest")),
             QStringLiteral("latest"));
}

void TestProfileEngine::setConfigIdentifiesTheActiveProfile()
{
    ProfileEngine engine;
    QSignalSpy active(&engine, &ProfileEngine::activeChanged);

    engine.setConfig(QStringLiteral("hd"), 12, 18, 3, QStringLiteral("latest"));
    QCOMPARE(engine.activeName(), QStringLiteral("High Quality"));
    QVERIFY(!engine.activeIsCustom());
    QVERIFY(!engine.activeEvidence().isEmpty());
    QCOMPARE(active.count(), 1);

    // Restating the same configuration is not a change.
    engine.setConfig(QStringLiteral("hd"), 12, 18, 3, QStringLiteral("latest"));
    QCOMPARE(active.count(), 1);

    engine.setConfig(QStringLiteral("vga"), 7, 18, 3, QStringLiteral("latest"));
    QVERIFY(engine.activeIsCustom());
    QCOMPARE(engine.activeName(), QStringLiteral("Custom"));

    // An empty resolution means the camera has not told us yet, and must not
    // overwrite a configuration already known.
    engine.setConfig(QString(), 12, 18, 3, QStringLiteral("latest"));
    QVERIFY(engine.activeIsCustom());
}

void TestProfileEngine::changingConfigurationResetsTheShortfallCount()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("hd"), 12, 18, 3, QStringLiteral("latest"));
    for (int i = 0; i < ProfileEngine::shortfallWindowsRequired(); ++i) {
        engine.observeFps(1.0);
    }
    QVERIFY(!engine.advice().isEmpty());

    // A new operating point is a new question; the old evidence does not carry
    // over and must not immediately condemn the new one.
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    QVERIFY(engine.advice().isEmpty());
}

void TestProfileEngine::aSingleSlowWindowDoesNotProduceAdvice()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    const int need = ProfileEngine::shortfallWindowsRequired();
    QVERIFY(need >= 2);

    // docs/benchmark-results.md records the same point measuring 19.65, 15.91
    // and 9.95 fps in one session as the scene changed. Acting on one window
    // would be reacting to daylight.
    engine.observeFps(4.0);
    QVERIFY2(engine.advice().isEmpty(), "one slow window was enough to advise a profile change");

    for (int i = 1; i < need - 1; ++i) {
        engine.observeFps(4.0);
    }
    QVERIFY2(engine.advice().isEmpty(), "advice appeared before the required run of windows");

    // One more window reaches the threshold and it does speak.
    engine.observeFps(4.0);
    QVERIFY(!engine.advice().isEmpty());
}

void TestProfileEngine::sustainedShortfallProducesAdviceNamingTheNextRung()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    for (int i = 0; i < ProfileEngine::shortfallWindowsRequired(); ++i) {
        engine.observeFps(4.0);
    }

    const QString advice = engine.advice();
    QVERIFY(!advice.isEmpty());
    // The advice has to be actionable: it names the profile to move to, the
    // measurement that triggered it and the floor it missed. The next rung
    // below Balanced is High FPS, not Low Latency - the ladder is ordered by
    // image quality, so a step down is always the adjacent entry.
    QVERIFY2(advice.contains(QStringLiteral("High FPS")), qPrintable(advice));
    QVERIFY2(advice.contains(QStringLiteral("4.0")), qPrintable(advice));
    QVERIFY2(advice.contains(QStringLiteral("15")), qPrintable(advice));
    QCOMPARE(engine.observedFps(), 4.0);
}

void TestProfileEngine::aRecoveredFrameRateClearsTheShortfall()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    engine.observeFps(4.0);
    engine.observeFps(4.0);
    engine.observeFps(4.0);
    QVERIFY(!engine.advice().isEmpty());

    // One healthy window is enough to forgive: the camera does not owe us the
    // rest of a bad run once it is back.
    engine.observeFps(22.5);
    QVERIFY(engine.advice().isEmpty());

    engine.clearObservation();
    QCOMPARE(engine.observedFps(), 0.0);
    QVERIFY(engine.advice().isEmpty());
}

void TestProfileEngine::manualModeNeverAdvises()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    for (int i = 0; i < ProfileEngine::shortfallWindowsRequired(); ++i) {
        engine.observeFps(1.0);
    }
    QVERIFY(!engine.advice().isEmpty());

    // The user chose this point. Second-guessing a deliberate choice on every
    // status refresh is noise, and the number is on screen anyway.
    engine.setModeIndex(2);
    QVERIFY(engine.advice().isEmpty());

    engine.setModeIndex(0);
    QVERIFY(!engine.advice().isEmpty());
}

void TestProfileEngine::theBottomRungHasNothingToOffer()
{
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("qvga"), 36, 18, 3, QStringLiteral("latest"));
    QCOMPARE(engine.activeName(), QStringLiteral("Low Latency"));
    for (int i = 0; i < ProfileEngine::shortfallWindowsRequired() + 3; ++i) {
        engine.observeFps(1.0);
    }
    // The lowest rung is already the lowest there is; suggesting a step down
    // from it would be a suggestion to do nothing, phrased as advice.
    QVERIFY(engine.advice().isEmpty());
}

void TestProfileEngine::closestByResolutionFindsTheNearestMeasuredPoint()
{
    // XGA is not in the ladder, so the advisory text needs the nearest point
    // that was actually measured rather than a guess. The argument is a
    // framesize key, the same vocabulary the ladder uses.
    const Profile *near = ProfileEngine::closestByResolution(QStringLiteral("xga"));
    QVERIFY(near);
    // 0.92 MP HD is the closest measured point to XGA's 0.79 MP.
    QVERIFY2(near->framesize == QStringLiteral("hd"), qPrintable(near->name));

    QVERIFY(ProfileEngine::closestByResolution(QStringLiteral("vga")));
    QVERIFY(ProfileEngine::closestByResolution(QStringLiteral("qqvga")));
    QVERIFY(ProfileEngine::closestByResolution(QStringLiteral("nonsense")) == nullptr);
}

QTEST_MAIN(TestProfileEngine)
#include "tst_profileengine.moc"
