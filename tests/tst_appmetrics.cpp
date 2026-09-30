// Master prompt section 33 asks for metric calculations, and the audit's
// section 14 listed the counters as the one functional area with no test at
// all: AppMetrics feeds the diagnostics overlay, the benchmark JSON and every
// published frame-rate figure, yet nothing pinned its arithmetic. These tests
// fix the numbers a reader of docs/performance.md is trusting - percentiles
// against a hand-computable distribution, the sample cap that must never lose
// a count, the counters themselves, the rate fields, and writeJson, which is
// how a benchmark run lands on disk.
#include "AppMetrics.h"

#include <QFile>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

class TestAppMetrics : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void resetClearsEveryCounterAndSeries();
    void seriesPercentilesMatchAKnownDistribution();
    void seriesCapDropsSamplesButKeepsCounting();
    void countersAccumulateUntilReset();
    void snapshotRatesAreFiniteAndNonNegative();
    void processCpuAndWorkingSetReportSaneValues();
    void writeJsonMergesExtraAndFailsOnAnImpossiblePath();
    void clockIsMonotonic();
};

void TestAppMetrics::init()
{
    // The singleton outlives each test function, so every test starts from
    // the same zero baseline instead of inheriting the previous test's data.
    AppMetrics::instance().reset();
}

void TestAppMetrics::resetClearsEveryCounterAndSeries()
{
    AppMetrics &m = AppMetrics::instance();

    m.countBytes(12345);
    m.countPart();
    m.countDecoded();
    m.countDecodeFailed();
    m.countOverwritten();
    m.countStaleDropped();
    m.countPresented();
    m.addDecodeUs(1000);

    m.reset();

    const QJsonObject snap = m.snapshot();
    QCOMPARE(snap.value(QStringLiteral("bytes_received")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("parts_parsed")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("frames_decoded")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("decode_failures")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("frames_overwritten")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("stale_dropped")).toInt(), 0);
    QCOMPARE(snap.value(QStringLiteral("frames_presented")).toInt(), 0);

    const QJsonObject decode = snap.value(QStringLiteral("decode_us")).toObject();
    QCOMPARE(decode.value(QStringLiteral("count")).toInt(), 0);
    QCOMPARE(decode.value(QStringLiteral("dropped_samples")).toInt(), 0);
    QVERIFY2(!decode.contains(QStringLiteral("p50")),
             "an empty series must not report percentiles of zero");
}

void TestAppMetrics::seriesPercentilesMatchAKnownDistribution()
{
    AppMetrics &m = AppMetrics::instance();

    // Ten samples, sorted: 10,20,...,100. The formula the implementation
    // documents is idx = int(p*(n-1)+0.5) into the sorted list:
    //   p50 -> idx 5  -> 60
    //   p95 -> idx 9  -> 100
    //   p99 -> idx 9  -> 100
    // mean = 550/10 = 55 (integer division, as implemented).
    for (int v = 10; v <= 100; v += 10) {
        m.addDecodeUs(v);
    }

    const QJsonObject decode =
        m.snapshot().value(QStringLiteral("decode_us")).toObject();
    QCOMPARE(decode.value(QStringLiteral("count")).toInt(), 10);
    QCOMPARE(decode.value(QStringLiteral("dropped_samples")).toInt(), 0);
    QCOMPARE(decode.value(QStringLiteral("min")).toInt(), 10);
    QCOMPARE(decode.value(QStringLiteral("max")).toInt(), 100);
    QCOMPARE(decode.value(QStringLiteral("p50")).toInt(), 60);
    QCOMPARE(decode.value(QStringLiteral("p95")).toInt(), 100);
    QCOMPARE(decode.value(QStringLiteral("p99")).toInt(), 100);
    QCOMPARE(decode.value(QStringLiteral("mean")).toInt(), 55);
}

void TestAppMetrics::seriesCapDropsSamplesButKeepsCounting()
{
    AppMetrics &m = AppMetrics::instance();

    // One past the retained window. The cap exists so a long soak cannot grow
    // the process without bound; what it must never do is lose the count -
    // every frame after the cap still has to show up in "count", while only
    // the surplus stops being retained for percentiles.
    constexpr qint64 kMax = 200000;
    for (qint64 i = 0; i < kMax + 1; ++i) {
        m.addParseUs(i);
    }

    const QJsonObject parse =
        m.snapshot().value(QStringLiteral("parse_us")).toObject();
    QCOMPARE(parse.value(QStringLiteral("count")).toInt(), kMax + 1);
    QCOMPARE(parse.value(QStringLiteral("dropped_samples")).toInt(), 1);
    // The retained window is still a full, valid distribution.
    QCOMPARE(parse.value(QStringLiteral("min")).toInt(), 0);
    QCOMPARE(parse.value(QStringLiteral("max")).toInt(), kMax - 1);
}

void TestAppMetrics::countersAccumulateUntilReset()
{
    AppMetrics &m = AppMetrics::instance();

    m.countBytes(1000);
    m.countBytes(2048);
    m.countPart();
    m.countPart();
    m.countPart();
    m.countDecoded();
    m.countDecodeFailed();
    m.countOverwritten();
    m.countStaleDropped();
    m.countPresented();
    m.addPresentAgeMs(7);
    m.addRenderAgeMs(3);
    m.addRenderUs(250);
    m.addScaleUs(90);
    m.addReceiveGapMs(41);

    const QJsonObject snap = m.snapshot();
    QCOMPARE(snap.value(QStringLiteral("bytes_received")).toInt(), 3048);
    QCOMPARE(snap.value(QStringLiteral("parts_parsed")).toInt(), 3);
    QCOMPARE(snap.value(QStringLiteral("frames_decoded")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("decode_failures")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("frames_overwritten")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("stale_dropped")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("frames_presented")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("present_age_ms")).toObject()
                 .value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("render_age_ms")).toObject()
                 .value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("render_us")).toObject()
                 .value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("scale_us")).toObject()
                 .value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(snap.value(QStringLiteral("receive_gap_ms")).toObject()
                 .value(QStringLiteral("count")).toInt(), 1);
}

void TestAppMetrics::snapshotRatesAreFiniteAndNonNegative()
{
    AppMetrics &m = AppMetrics::instance();

    // The rate fields are wall-clock divisions, so they only exist once a
    // millisecond has passed; the test cannot pin their exact value because
    // the wall clock keeps running, but it can pin that every rate that does
    // appear is a number a reader could act on: finite, non-negative, and
    // coupled to the counters that produced it.
    m.countPart();
    QTest::qWait(5);
    m.countPart();

    const QJsonObject snap = m.snapshot();
    QVERIFY(snap.value(QStringLiteral("uptime_ms")).toInt() >= 0);

    const QStringList rates{QStringLiteral("parts_fps"), QStringLiteral("decoded_fps"),
                            QStringLiteral("presented_fps"),
                            QStringLiteral("receive_mbps")};
    for (const QString &key : rates) {
        if (!snap.contains(key)) {
            // wall == 0 is legal on the very first snapshot after reset.
            continue;
        }
        const double v = snap.value(key).toDouble();
        QVERIFY2(std::isfinite(v), qPrintable(key + " is not finite"));
        QVERIFY2(v >= 0.0, qPrintable(key + " is negative"));
    }
}

void TestAppMetrics::processCpuAndWorkingSetReportSaneValues()
{
    AppMetrics &m = AppMetrics::instance();

    // -1.0 is the documented "not available" answer (GetProcessTimes failed or
    // no wall time yet); anything else must be a percentage, not garbage.
    const double cpu = m.processCpuPercent();
    QVERIFY(cpu >= -1.0);
    if (cpu >= 0.0) {
        QVERIFY(std::isfinite(cpu));
    }
    QVERIFY2(m.workingSetBytes() > 0, "the working set could not be read");
}

void TestAppMetrics::writeJsonMergesExtraAndFailsOnAnImpossiblePath()
{
    AppMetrics &m = AppMetrics::instance();
    m.countBytes(4096);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString good = dir.filePath(QStringLiteral("metrics.json"));

    QJsonObject extra;
    extra[QStringLiteral("bench_label")] = QStringLiteral("unit-test");
    QVERIFY2(m.writeJson(good, extra), qPrintable(good));

    QFile f(good);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject written =
        QJsonDocument::fromJson(f.readAll()).object();
    QVERIFY2(!written.isEmpty(), "the written file is not a JSON object");
    // The merge must add, not replace: benchmark metadata rides alongside the
    // metrics, never instead of them.
    QCOMPARE(written.value(QStringLiteral("bench_label")).toString(),
             QStringLiteral("unit-test"));
    QCOMPARE(written.value(QStringLiteral("bytes_received")).toInt(), 4096);

    // A path whose parent does not exist (and is not created for the caller)
    // must report failure - the benchmark harness decides whether that is a
    // stop, so a silent false would lose the whole run's data file.
    const QString impossible =
        dir.filePath(QStringLiteral("missing/parent/metrics.json"));
    QVERIFY2(!m.writeJson(impossible, extra),
             "writeJson claimed success for an unopenable path");
}

void TestAppMetrics::clockIsMonotonic()
{
    // The benchmark harness derives durations from nowMs()/nowUs(); a clock
    // that ever moved backwards would produce negative elapsed times and
    // quietly poisoned percentiles.
    const qint64 a = AppMetrics::nowMs();
    const qint64 b = AppMetrics::nowMs();
    QVERIFY(b >= a);
    const qint64 ua = AppMetrics::nowUs();
    const qint64 ub = AppMetrics::nowUs();
    QVERIFY(ub >= ua);
    QVERIFY(ua >= 0);
}

QTEST_MAIN(TestAppMetrics)
#include "tst_appmetrics.moc"
