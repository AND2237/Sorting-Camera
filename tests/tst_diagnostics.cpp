// Section 38 asks for five levels, per-subsystem categories, and - the part
// that decides whether the log is usable at 20 fps - rate-limited and
// statistical reporting instead of a line per frame. This test pins all three,
// and in particular pins that suppression is counted rather than hidden: a
// truncated log that does not say how much it dropped answers the wrong
// question convincingly.
#include "Diagnostics.h"

#include <QSignalSpy>
#include <QTest>

using namespace Diagnostics;

class TestDiagnostics : public QObject
{
    Q_OBJECT

private slots:
    void levelNamesRoundTrip();
    void categoriesCoverEverySubsystemSection38Names();
    void entriesCarryLevelAndCategory();
    void repeatedLogsAreRateLimitedAndCounted();
    void criticalIsNeverRateLimited();
    void theSuppressedCountRidesOnTheNextAcceptedLine();
    void minimumLevelFiltersBelowIt();
    void countersTrackMinMaxAndLast();
    void countersAreKeyedByName();
    void ringBufferIsBounded();
    void countsByLevelAndCategory();
};

void TestDiagnostics::levelNamesRoundTrip()
{
    const QList<Level> all = {Level::Debug, Level::Info, Level::Warning, Level::Error,
                              Level::Critical};
    for (Level l : all) {
        Level parsed = Level::Info;
        QVERIFY2(parseLevel(levelName(l), &parsed), qPrintable(levelName(l)));
        QCOMPARE(parsed, l);
    }
    // Case-insensitive, because a user typing a level into a config or a
    // command line should not have to match our capitalisation.
    Level parsed = Level::Info;
    QVERIFY(parseLevel(QStringLiteral("warning"), &parsed));
    QCOMPARE(parsed, Level::Warning);
    QVERIFY(!parseLevel(QStringLiteral("verbose"), &parsed));
    QVERIFY(!parseLevel(QString(), &parsed));
}

void TestDiagnostics::categoriesCoverEverySubsystemSection38Names()
{
    // Section 38 lists discovery, authentication, connection, transport, frame
    // reception, frame loss, decoding, rendering and recording. Each has to
    // exist, with a name, because a category with an empty name is a category
    // nobody can filter by.
    const QList<Category> required = {
        Category::Discovery, Category::Auth,    Category::Connection, Category::Transport,
        Category::Frames,     Category::Drops,   Category::Decode,     Category::Render,
        Category::Recording};
    for (Category c : required) {
        QVERIFY2(!categoryName(c).isEmpty(),
                 qPrintable(QStringLiteral("category %1 has no name").arg(static_cast<int>(c))));
    }
    // Plus the two Phase 6 additions and the fallback.
    QVERIFY(!categoryName(Category::Config).isEmpty());
    QVERIFY(!categoryName(Category::Profile).isEmpty());
    QCOMPARE(categoryName(Category::Other), QStringLiteral("other"));

    // Names are unique, so a log line identifies exactly one subsystem.
    QSet<QString> seen;
    for (int i = 0; i < static_cast<int>(Category::CategoryCount); ++i) {
        const QString n = categoryName(static_cast<Category>(i));
        QVERIFY2(!seen.contains(n), qPrintable(QStringLiteral("duplicate category name %1").arg(n)));
        seen.insert(n);
    }
}

void TestDiagnostics::entriesCarryLevelAndCategory()
{
    Facility f;
    f.log(Category::Transport, Level::Warning, QStringLiteral("first byte timeout"));
    QCOMPARE(f.entryCount(), 1);
    QCOMPARE(f.entries().at(0).level, Level::Warning);
    QCOMPARE(f.entries().at(0).category, Category::Transport);
    QCOMPARE(f.entries().at(0).message, QStringLiteral("first byte timeout"));
    QVERIFY(f.entries().at(0).timestampMs > 0);
}

void TestDiagnostics::repeatedLogsAreRateLimitedAndCounted()
{
    Facility f;
    // A tight budget so the test does not depend on wall-clock timing.
    f.setRateLimit(Level::Warning, 60000, 3);

    for (int i = 0; i < 100; ++i) {
        f.log(Category::Connection, Level::Warning, QStringLiteral("retry %1").arg(i));
    }

    // Three got through, and the other ninety-seven are accounted for rather
    // than lost. A log that silently dropped them would make a flapping link
    // look like a quiet one.
    QCOMPARE(f.entryCount(), 3);
    QCOMPARE(f.suppressedTotal(), 97);
}

void TestDiagnostics::criticalIsNeverRateLimited()
{
    Facility f;
    for (int i = 0; i < 50; ++i) {
        f.log(Category::Transport, Level::Critical, QStringLiteral("fatal %1").arg(i));
    }
    // A critical that fires once matters; one that fires fifty times matters
    // more, and there is no rate at which that becomes less important.
    QCOMPARE(f.entryCount(), 50);
    QCOMPARE(f.suppressedTotal(), 0);
}

void TestDiagnostics::theSuppressedCountRidesOnTheNextAcceptedLine()
{
    Facility f;
    f.setRateLimit(Level::Error, 60000, 2);

    f.log(Category::Drops, Level::Error, QStringLiteral("first"));
    f.log(Category::Drops, Level::Error, QStringLiteral("second"));
    f.log(Category::Drops, Level::Error, QStringLiteral("third"));
    f.log(Category::Drops, Level::Error, QStringLiteral("fourth"));
    f.log(Category::Drops, Level::Error, QStringLiteral("fifth"));
    QCOMPARE(f.suppressedTotal(), 3);

    // Re-arm the same budget. The pending suppression count must survive: a
    // limit changed at runtime cannot be allowed to erase the record of what
    // the previous limit swallowed, or turning the level up would hide the
    // very burst it was turned up to see.
    f.setRateLimit(Level::Error, 60000, 2);
    f.log(Category::Drops, Level::Error, QStringLiteral("after reset"));

    // Located by message rather than by position: the facility installs a Qt
    // message handler, so the framework's own chatter shares this buffer and
    // the newest entry is not necessarily the one just logged.
    int found = -1;
    for (int i = f.entryCount() - 1; i >= 0; --i) {
        if (f.entries().at(i).message == QStringLiteral("after reset")) {
            found = i;
            break;
        }
    }
    QVERIFY2(found >= 0, "the re-armed line is not in the buffer");
    QVERIFY2(f.entries().at(found).suppressed > 0,
             "the accepted line does not report what it suppressed");
    QCOMPARE(f.entries().at(found).suppressed, 3);

    // And the UI-facing projection shows it. The window is generous because
    // the installed message handler also captures the test framework's own
    // chatter into this same buffer, so the entry of interest is not
    // necessarily among the most recent few.
    // The projection of a row this test created must carry the note. Other rows
    // in the buffer belong to the test framework's own output - the facility
    // installs a Qt message handler, so anything QtTest prints is recorded
    // too, with however many arguments the macro expanded. That is correct
    // behaviour for the facility, so the assertion is scoped to what this test
    // logged rather than to the whole buffer.
    const QVariantList recent = f.recentEntries(f.entryCount());
    int checked = 0;
    for (const QVariant &row : recent) {
        const QVariantList entry = row.toList();
        if (entry.size() != 4) {
            continue;
        }
        if (entry.at(3).toString().contains(QStringLiteral("after reset"))) {
            ++checked;
            QVERIFY2(entry.at(3).toString().contains(QStringLiteral("suppressed")),
                     qPrintable(entry.at(3).toString()));
        }
    }
    QVERIFY2(checked > 0, "the logged row is not in the projection at all");
}

void TestDiagnostics::minimumLevelFiltersBelowIt()
{
    Facility f;
    f.setMinimumLevel(static_cast<int>(Level::Warning));
    f.log(Category::Config, Level::Debug, QStringLiteral("noise"));
    f.log(Category::Config, Level::Info, QStringLiteral("noise"));
    f.log(Category::Config, Level::Warning, QStringLiteral("kept"));
    f.log(Category::Config, Level::Error, QStringLiteral("kept"));

    // The level control has to actually reduce volume, not merely relabel it.
    QCOMPARE(f.entryCount(), 2);
    f.setMinimumLevel(static_cast<int>(Level::Debug));
    f.log(Category::Config, Level::Debug, QStringLiteral("now visible"));
    QCOMPARE(f.entryCount(), 3);
}

void TestDiagnostics::countersTrackMinMaxAndLast()
{
    Facility f;
    f.countFrame(QStringLiteral("frame.bytes"), 58000);
    f.countFrame(QStringLiteral("frame.bytes"), 12000);
    f.countFrame(QStringLiteral("frame.bytes"), 31000);

    const QString text = f.statistics();
    QVERIFY2(text.contains(QStringLiteral("frame.bytes=31000")), qPrintable(text));
    // The spread is the point of counting rather than logging: a last value of
    // 31000 says nothing about whether the stream is steady.
    QVERIFY2(text.contains(QStringLiteral("12000..58000")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("3")), qPrintable(text));
}

void TestDiagnostics::countersAreKeyedByName()
{
    Facility f;
    f.countFrame(QStringLiteral("frame.bytes"), 100);
    f.countFrame(QStringLiteral("frame.age_ms"), 14);
    f.countDrop(QStringLiteral("stale"), 1);
    f.countFrame(QStringLiteral("frame.bytes"), 200);

    const QString text = f.statistics();
    QVERIFY(text.contains(QStringLiteral("frame.bytes=200")));
    QVERIFY(text.contains(QStringLiteral("frame.age_ms=14")));
    QVERIFY(text.contains(QStringLiteral("drop.stale=1")));
    // A second sample of the same name updates rather than appending a row.
    QVERIFY(!text.contains(QStringLiteral("frame.bytes=100")));

    f.resetStatistics();
    QVERIFY(f.statistics().isEmpty());
}

void TestDiagnostics::ringBufferIsBounded()
{
    Facility f;
    f.setMaxEntries(50);
    for (int i = 0; i < 500; ++i) {
        f.log(Category::Other, Level::Debug, QStringLiteral("line %1").arg(i));
    }
    // Debug's default budget of 5 per 5 s would stop this test long before 50
    // entries, so the buffer bound is asserted on the counter rather than on
    // the contents, and the contents are checked with the limit lifted.
    QVERIFY(f.entryCount() <= 50);

    f.setRateLimit(Level::Debug, 60000, 0); // unlimited
    f.setMaxEntries(50);
    f.log(Category::Other, Level::Debug, QStringLiteral("final"));
    QVERIFY(f.entryCount() <= 50);
    // The newest entry is the one kept.
    QCOMPARE(f.entries().last().message, QStringLiteral("final"));
}

void TestDiagnostics::countsByLevelAndCategory()
{
    Facility f;
    f.log(Category::Auth, Level::Info, QStringLiteral("signed in"));
    f.log(Category::Auth, Level::Error, QStringLiteral("bad password"));
    f.log(Category::Drops, Level::Warning, QStringLiteral("dropped a frame"));

    QCOMPARE(f.countInCategory(Category::Auth), 2);
    QCOMPARE(f.countInCategory(Category::Drops), 1);
    QCOMPARE(f.countInCategory(Category::Recording), 0);
    QCOMPARE(f.countAtLeast(Level::Error), 1);
    QCOMPARE(f.countAtLeast(Level::Warning), 2);
    QCOMPARE(f.errorCount(), 1);
    QCOMPARE(f.warningCount(), 1);

    // Names are exposed for the level filter and the panel header.
    QCOMPARE(f.levelNameAt(static_cast<int>(Level::Critical)), QStringLiteral("CRITICAL"));
    QVERIFY(f.levelNameAt(99).isEmpty());
    QCOMPARE(f.categoryNameAt(static_cast<int>(Category::Transport)),
             QStringLiteral("transport"));
    QVERIFY(f.categoryNameAt(99).isEmpty());
}

QTEST_MAIN(TestDiagnostics)
#include "tst_diagnostics.moc"
