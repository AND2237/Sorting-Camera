// Section 12 asks for profiles that optimise by image quality > FPS > latency,
// and says the useful operating points have to come from experiment rather than
// assumption. So this test checks two separate things: that the ladder is
// ordered by the project's own priority, and that Automatic's answer follows
// from the data instead of being hard-coded to a profile id.
#include "ProfileEngine.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using Profile = ProfileEngine::Profile;
using Id = ProfileEngine::ProfileId;

namespace {

using Citation = Profile::Citation;

// Where the committed benchmark artifacts live, as told to the build.
QString benchDir()
{
    return QString::fromUtf8(SCAM_BENCH_DIR);
}

// The ladder speaks framesize keys; the artifacts speak camera resolutions.
// The D2 run report already uses keys, so both forms have to be accepted.
QString keyFrom(const QString &resolutionOrKey)
{
    return resolutionOrKey.contains(QLatin1Char('x'))
        ? ProfileEngine::framesizeKeyFromResolution(resolutionOrKey)
        : resolutionOrKey;
}

struct Resolved
{
    bool found = false;
    QString resolution;
    int quality = 0;
    int fbCount = 0;
    double fps = 0.0;
    double bytes = 0.0;
    QString error;
};

// Reads back one citation exactly as the harness wrote it. Two artifact
// shapes are supported: the cell matrix (one JSON object per line, keyed by
// stage/resolution/quality/fb_count) and the run report (one object, whose
// fps is the three-way measurement and whose frame size is bytes over frames).
Resolved resolve(const Citation &c)
{
    Resolved r;
    const QString path = benchDir() + QLatin1Char('/') + c.artifact;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("cannot open %1").arg(path);
        return r;
    }

    if (c.artifact.endsWith(QLatin1String(".jsonl"))) {
        while (!f.atEnd()) {
            const QByteArray line = f.readLine();
            if (line.trimmed().isEmpty()) {
                continue;
            }
            const QJsonObject root = QJsonDocument::fromJson(line).object();
            if (root.isEmpty()) {
                continue;
            }
            const QJsonObject meta = root.value(QStringLiteral("meta")).toObject();
            const QJsonObject cfg = root.value(QStringLiteral("config_echo")).toObject();
            if (meta.value(QStringLiteral("stage")).toString() != c.stage
                || cfg.value(QStringLiteral("resolution")).toString() != c.resolution
                || cfg.value(QStringLiteral("quality")).toInt() != c.quality
                || cfg.value(QStringLiteral("fb_count")).toInt() != c.fbCount) {
                continue;
            }
            const QJsonObject rx = root.value(QStringLiteral("rx")).toObject();
            r.fps = rx.value(QStringLiteral("fps")).toObject().value(QStringLiteral("mean")).toDouble();
            r.bytes = rx.value(QStringLiteral("frame_bytes")).toObject()
                          .value(QStringLiteral("p50")).toDouble();
            r.resolution = c.resolution;
            r.quality = c.quality;
            r.fbCount = c.fbCount;
            r.found = true;
            return r;
        }
        r.error = QStringLiteral("no %1 cell for %2 q%3 fb%4 in %5")
                      .arg(c.stage, c.resolution)
                      .arg(c.quality)
                      .arg(c.fbCount)
                      .arg(c.artifact);
        return r;
    }

    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject meta = root.value(QStringLiteral("meta")).toObject();
    const QJsonObject app = root.value(QStringLiteral("app")).toObject();
    const QJsonObject dev = root.value(QStringLiteral("device_summary")).toObject();
    if (meta.value(QStringLiteral("requested_framesize")).toString() != c.resolution
        || app.value(QStringLiteral("requested_quality")).toInt() != c.quality
        || app.value(QStringLiteral("requested_fb_count")).toInt() != c.fbCount) {
        r.error = QStringLiteral("%1 does not describe %2 q%3 fb%4")
                      .arg(c.artifact, c.resolution)
                      .arg(c.quality)
                      .arg(c.fbCount);
        return r;
    }
    const double frames = app.value(QStringLiteral("frames_presented")).toDouble();
    // The same three-way measurement docs/benchmark-results.md quotes as
    // "9.96 / 9.97 / 10.00 fps", and the same byte arithmetic behind the
    // "28,956 B per frame" it publishes.
    r.fps = (dev.value(QStringLiteral("capture_fps")).toDouble()
             + dev.value(QStringLiteral("delivery_fps")).toDouble()
             + app.value(QStringLiteral("decoded_fps")).toDouble())
        / 3.0;
    r.bytes = frames > 0.0
        ? app.value(QStringLiteral("bytes_received")).toDouble() / frames
        : 0.0;
    r.resolution = c.resolution;
    r.quality = c.quality;
    r.fbCount = c.fbCount;
    r.found = true;
    return r;
}

// Digits grouped in threes, the way every byte figure is written in
// docs/benchmark-results.md and in the evidence strings.
QString thousands(double v)
{
    const QString s = QString::number(qRound64(v));
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) {
            out += QLatin1Char(',');
        }
        out += s.at(i);
    }
    return out;
}

// Published figures carry two decimals, so two numbers are the same published
// figure exactly when they round to the same hundredth. A tolerance of 0.01 is
// the wrong test: it admits 9.98 as equal to 9.97, which is a different number
// in the table the user reads.
bool sameFps(double a, double b)
{
    return qRound(a * 100.0) == qRound(b * 100.0);
}

// Byte figures are whole bytes in every artifact, so they match exactly.
bool sameBytes(double a, double b)
{
    return qRound64(a) == qRound64(b);
}

} // namespace

class TestProfileEngine : public QObject
{
    Q_OBJECT

private slots:
    void ladderIsOrderedByImageQualityFirst();
    void everyLadderEntryIsAMeasuredOperatingPoint();
    void everyPublishedFigureExistsInTheArtifactItCites();
    void noPublishedFigureComesFromAnotherConfiguration();
    void everyLadderFigureIsCoveredByACitation();
    void evidenceQuotesTheFiguresItCites();
    void theConservativeReadingDrivesTheAutomaticChoice();
    void onlyARecordedDecisionCarriesAReducedFloor();
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

void TestProfileEngine::everyPublishedFigureExistsInTheArtifactItCites()
{
    QVERIFY2(QDir(benchDir()).exists(),
             qPrintable(QStringLiteral(
                 "benchmark artifacts not found at %1 - every figure in the ladder cites "
                 "them, and a ladder that cannot be checked against its own source is a "
                 "ladder nobody can audit").arg(benchDir())));

    for (const Profile &p : ProfileEngine::ladder()) {
        QVERIFY2(!p.citations.isEmpty(),
                 qPrintable(p.name + " publishes figures but cites no artifact"));
        for (const Citation &c : p.citations) {
            const Resolved r = resolve(c);
            QVERIFY2(r.found, qPrintable(p.name + ": " + r.error));
            QVERIFY2(sameFps(r.fps, c.fps),
                     qPrintable(QStringLiteral(
                         "%1 cites %2 %3 q%4 as %5 fps, but %6 records %7")
                                    .arg(p.name, c.artifact, c.stage, c.resolution)
                                    .arg(c.fps)
                                    .arg(QString::number(c.fps, 'f', 2))
                                    .arg(QString::number(r.fps, 'f', 2))));
            QVERIFY2(sameBytes(r.bytes, c.bytes),
                     qPrintable(QStringLiteral(
                         "%1 cites %2 %3 q%4 as %5 B, but the artifact records %6")
                                    .arg(p.name, c.artifact, c.stage, c.resolution)
                                    .arg(thousands(c.bytes))
                                    .arg(thousands(r.bytes))));
        }
    }
}

void TestProfileEngine::noPublishedFigureComesFromAnotherConfiguration()
{
    // The defect this guards: svga/q24 carried vga/q24's byte count, and a
    // comment on the same profile described vga/q24's confirm run as "the more
    // conservative reading of the same point". Both are the ladder speaking
    // about one configuration using another's measurements, so the check is
    // made against the artifact's own resolution and quality rather than
    // against whatever the citation claims.
    for (const Profile &p : ProfileEngine::ladder()) {
        for (const Citation &c : p.citations) {
            const Resolved r = resolve(c);
            QVERIFY2(r.found, qPrintable(p.name + ": " + r.error));
            QVERIFY2(keyFrom(r.resolution) == p.framesize,
                     qPrintable(QStringLiteral(
                         "%1 (%2) publishes a figure measured at %3")
                                    .arg(p.name, p.framesize, r.resolution)));
            QCOMPARE(r.quality, p.quality);
        }
    }

    // And the specific number that was borrowed, pinned against its own cell.
    const Profile *balanced = ProfileEngine::byId(Id::Balanced);
    QVERIFY(balanced);
    QVERIFY2(balanced->medianBytes != 14680.0,
             "14,680 B is vga/q24's confirm figure and has no business on an svga profile");
    QCOMPARE(balanced->medianBytes, 16982.0);
    QVERIFY2(balanced->evidence.contains(QStringLiteral("16,982")),
             qPrintable(balanced->evidence));
    QVERIFY2(!balanced->evidence.contains(QStringLiteral("18.97")),
             "vga/q24's confirm fps was quoted as if it were svga/q24's");

    // Two entries at different resolutions may not publish the same byte count.
    // The borrowed value was not merely mislabelled, it made two rungs of the
    // ladder claim identical frame sizes at four times the pixels, which is the
    // shape of the error rather than the number itself.
    const QList<Profile> all = ProfileEngine::ladder();
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].medianBytes == 0.0) {
            continue;
        }
        for (int j = i + 1; j < all.size(); ++j) {
            if (all[j].medianBytes == 0.0
                || all[j].framesize == all[i].framesize) {
                continue;
            }
            QVERIFY2(all[i].medianBytes != all[j].medianBytes,
                     qPrintable(QStringLiteral(
                         "%1 (%2) and %3 (%4) both publish %5 B")
                                    .arg(all[i].name, all[i].framesize,
                                         all[j].name, all[j].framesize)
                                    .arg(thousands(all[i].medianBytes))));
        }
    }
}

void TestProfileEngine::everyLadderFigureIsCoveredByACitation()
{
    // A citation nobody publishes is decoration; a published figure with no
    // citation is a rumour. Both directions are required to hold.
    for (const Profile &p : ProfileEngine::ladder()) {
        bool measuredCovered = false;
        bool altCovered = p.altMeasuredFps == 0.0;
        bool bytesCovered = p.medianBytes == 0.0;
        for (const Citation &c : p.citations) {
            if (sameFps(c.fps, p.measuredFps)) {
                measuredCovered = true;
            }
            if (!altCovered && sameFps(c.fps, p.altMeasuredFps)) {
                altCovered = true;
            }
            if (!bytesCovered && sameBytes(c.bytes, p.medianBytes)) {
                bytesCovered = true;
            }
        }
        QVERIFY2(measuredCovered,
                 qPrintable(p.name + "'s measuredFps is not among the figures it cites"));
        QVERIFY2(altCovered,
                 qPrintable(p.name + "'s altMeasuredFps is not among the figures it cites"));
        QVERIFY2(bytesCovered,
                 qPrintable(p.name + "'s medianBytes is not among the figures it cites"));
    }
}

void TestProfileEngine::evidenceQuotesTheFiguresItCites()
{
    // The evidence string is what the user actually reads at the bottom of the
    // profile panel, so it has to carry the numbers the citations resolve to.
    // This is what catches a correct number sitting behind a false sentence.
    for (const Profile &p : ProfileEngine::ladder()) {
        for (const Citation &c : p.citations) {
            const QString fps = QString::number(c.fps, 'f', 2);
            QVERIFY2(p.evidence.contains(fps),
                     qPrintable(QStringLiteral("%1's evidence never mentions %2 fps from %3")
                                    .arg(p.name, fps, c.stage)));
            QVERIFY2(p.evidence.contains(thousands(c.bytes)),
                     qPrintable(QStringLiteral("%1's evidence never mentions %2 B from %3")
                                    .arg(p.name, thousands(c.bytes), c.stage)));
        }
    }
}

void TestProfileEngine::theConservativeReadingDrivesTheAutomaticChoice()
{
    // The contract of conservativeFps itself.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (p.altMeasuredFps > 0.0) {
            QCOMPARE(ProfileEngine::conservativeFps(p),
                     qMin(p.altMeasuredFps, p.measuredFps));
            QVERIFY(ProfileEngine::conservativeFps(p) <= p.measuredFps);
        } else {
            QCOMPARE(ProfileEngine::conservativeFps(p), p.measuredFps);
        }
    }

    // 0 means "no second reading was admitted", never "measured zero". If it
    // were read as a measurement, every profile without a confirm reading
    // would sit at 0 fps and fall out of Automatic entirely.
    const Profile *balanced = ProfileEngine::byId(Id::Balanced);
    QVERIFY(balanced);
    QCOMPARE(balanced->altMeasuredFps, 0.0);
    QCOMPARE(ProfileEngine::conservativeFps(*balanced), balanced->measuredFps);

    // The shipped table cannot separate the conservative rule from the
    // optimistic one: every admitted second reading clears its floor, so both
    // rules pick the same profile. So the rule is exercised against a ladder
    // shaped so the two rules disagree.
    const Profile *hq = ProfileEngine::byId(Id::HighQuality);
    QVERIFY(hq);

    QList<Profile> optimistic = ProfileEngine::ladder();
    for (Profile &p : optimistic) {
        if (p.id == Id::HighQuality) {
            p.altMeasuredFps = 6.0; // under HD's 7 fps floor
        }
    }
    const Profile *winner = ProfileEngine::recommendedIn(optimistic);
    QVERIFY(winner);
    QVERIFY2(winner->id != Id::HighQuality,
             qPrintable(QStringLiteral(
                 "recommended() ignored the conservative reading: HD's admitted "
                 "second reading is 6.0 fps against a 7 fps floor, yet %1 was chosen")
                            .arg(winner->name)));
    QCOMPARE(winner->id, Id::Balanced);

    // The same profile with that reading not admitted is not affected: 0 is a
    // missing measurement, and a missing measurement must not fail a floor.
    QList<Profile> unadmitted = ProfileEngine::ladder();
    for (Profile &p : unadmitted) {
        if (p.id == Id::HighQuality) {
            p.altMeasuredFps = 0.0;
        }
    }
    QCOMPARE(ProfileEngine::recommendedIn(unadmitted)->id, Id::HighQuality);

    // And with the shipped table, the answer is unchanged.
    QCOMPARE(ProfileEngine::recommendedIn(ProfileEngine::ladder())->id, Id::HighQuality);

    // The floor verdict the UI shows has to be drawn from the same rule as the
    // recommendation, or the panel could say a profile meets its floor while
    // Automatic declines to pick it. Today's table cannot tell the conservative
    // rule apart from the optimistic one - every admitted second reading clears
    // its floor - so this pins the two together anyway rather than waiting for
    // a measurement that separates them.
    for (const Profile &p : ProfileEngine::ladder()) {
        ProfileEngine engine;
        engine.setConfig(p.framesize, p.quality, p.xclkMhz, p.frameBufferCount, p.grabMode);
        QCOMPARE(engine.activeMeetsFloor(),
                 ProfileEngine::conservativeFps(p) >= p.floorFps);
    }
}

void TestProfileEngine::onlyARecordedDecisionCarriesAReducedFloor()
{
    // ADR-0010 set HD's floor at >=7 fps and called it provisional: the >=1 h
    // soak has not run, so nothing has replaced it with a sustained figure.
    // A reduced floor presented as settled is the defect - the number itself
    // is the owner's decision and is correct.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (p.floorProvisional) {
            QCOMPARE(p.id, Id::HighQuality);
            QVERIFY(p.floorFps < 15.0);
            QVERIFY2(p.evidence.contains(QStringLiteral("provisional")),
                     qPrintable(p.name + " hides a provisional floor from the user"));
        }
    }

    // The converse is the actual guard: any floor below the general 15 has to
    // say it is provisional, or the UI will render 7.0 as a settled target.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (p.floorFps < 15.0) {
            QVERIFY2(p.floorProvisional,
                     qPrintable(QStringLiteral(
                         "%1 carries a reduced %2 fps floor that is not marked provisional")
                                    .arg(p.name)
                                    .arg(p.floorFps)));
        }
    }

    // And the exposed flag a UI would use follows the model.
    ProfileEngine engine;
    engine.setConfig(QStringLiteral("hd"), 12, 18, 3, QStringLiteral("latest"));
    QVERIFY(engine.activeFloorProvisional());
    engine.setConfig(QStringLiteral("svga"), 24, 18, 3, QStringLiteral("latest"));
    QVERIFY(!engine.activeFloorProvisional());
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
    // recommendation. Judged the same way recommended() judges it - on the
    // conservative reading - so this stays a mirror rather than a second rule.
    for (const Profile &p : ProfileEngine::ladder()) {
        if (ProfileEngine::conservativeFps(p) >= p.floorFps) {
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
