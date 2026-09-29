#include "ConfigWriteSequence.h"

#include <QTest>

class TestConfigWriteSequence : public QObject
{
    Q_OBJECT

private slots:
    void walksEveryRequestedSetting();
    void aGapDoesNotTruncateTheSequence();
    void anEmptyRunHasNothingToDo();
    void takeNextStopsAtTheEnd();
};

void TestConfigWriteSequence::walksEveryRequestedSetting()
{
    // framesize, quality, xclk, frame-buffer count, grab mode - all asked for.
    ConfigWriteSequence seq({0, 1, 2, 3, 4});
    QCOMPARE(seq.total(), 5);
    for (int expected = 0; expected < 5; ++expected) {
        QVERIFY(seq.hasNext());
        QCOMPARE(seq.takeNext(), expected);
    }
    QVERIFY(!seq.hasNext());
    QCOMPARE(seq.takeNext(), -1);
    QCOMPARE(seq.applied(), 5);
}

void TestConfigWriteSequence::aGapDoesNotTruncateTheSequence()
{
    // `--quality` without `--framesize`. The chain used to be indexed, so the
    // unset framesize slot returned "nothing to apply", the caller took that
    // for the end of the list, and the quality write was never issued while
    // the result file still recorded it (CP-12).
    ConfigWriteSequence seq({1, 2});
    QCOMPARE(seq.total(), 2);
    QVERIFY(seq.hasNext());
    QCOMPARE(seq.takeNext(), 1);
    QVERIFY(seq.hasNext());
    QCOMPARE(seq.takeNext(), 2);
    QVERIFY(!seq.hasNext());
    QCOMPARE(seq.takeNext(), -1);

    // Nothing earlier may block a setting asked for on its own.
    ConfigWriteSequence middle({2});
    QCOMPARE(middle.takeNext(), 2);
    QCOMPARE(middle.takeNext(), -1);

    // Nor may anything later: a trailing gap must not stop the first write.
    ConfigWriteSequence leading({0});
    QCOMPARE(leading.takeNext(), 0);
    QVERIFY(!leading.hasNext());
}

void TestConfigWriteSequence::anEmptyRunHasNothingToDo()
{
    ConfigWriteSequence seq;
    QCOMPARE(seq.total(), 0);
    QCOMPARE(seq.applied(), 0);
    QVERIFY(!seq.hasNext());
    // The caller schedules the stream as soon as there is nothing to write.
    QCOMPARE(seq.takeNext(), -1);
}

void TestConfigWriteSequence::takeNextStopsAtTheEnd()
{
    ConfigWriteSequence seq({0, 4});
    QCOMPARE(seq.takeNext(), 0);
    QCOMPARE(seq.takeNext(), 4);
    // Asking past the end must not wrap, repeat or corrupt the count: the busy
    // handler re-enters this path every time a write finishes.
    QCOMPARE(seq.takeNext(), -1);
    QCOMPARE(seq.takeNext(), -1);
    QCOMPARE(seq.applied(), 2);
    QCOMPARE(seq.total(), 2);
}

QTEST_GUILESS_MAIN(TestConfigWriteSequence)
#include "tst_configwritesequence.moc"
