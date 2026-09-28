// Section 26 requires error and status notifications. The behaviours worth
// testing are the ones a user would notice going wrong: the same fault
// repeating must not bury the list, an error must not quietly expire before it
// has been read, and a condition that has been fixed must not leave a stale
// card behind.
#include "NotificationCenter.h"

#include <QSignalSpy>
#include <QTest>

using namespace Diagnostics;

class TestNotificationCenter : public QObject
{
    Q_OBJECT

private slots:
    void postedItemsAppearNewestFirst();
    void rolesCarrySeverityCategoryAndText();
    void infoExpiresAndErrorDoesNot();
    void repeatingTheSameKeyDoesNotGrowTheList();
    void repeatingTheSameKeyRefreshesAndMovesToTop();
    void dismissRemovesOneRow();
    void dismissAllClearsEverything();
    void countsTrackSeverity();
    void stickyIsWarningAndAbove();
    void emptyTextIsNotPosted();
    void distinctKeysAccumulate();
};

void TestNotificationCenter::postedItemsAppearNewestFirst()
{
    NotificationCenter n;
    QSignalSpy posted(&n, &NotificationCenter::posted);
    n.post(Level::Error, Category::Config, QStringLiteral("first"));
    n.post(Level::Warning, Category::Drops, QStringLiteral("second"));

    QCOMPARE(n.rowCount(), 2);
    // Newest first: the thing that just happened is the thing being read.
    QCOMPARE(n.data(n.index(0, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("second"));
    QCOMPARE(n.data(n.index(1, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("first"));
    QCOMPARE(posted.count(), 2);
}

void TestNotificationCenter::rolesCarrySeverityCategoryAndText()
{
    NotificationCenter n;
    n.post(Level::Critical, Category::Transport, QStringLiteral("socket closed"));

    const QModelIndex i = n.index(0, 0);
    QCOMPARE(n.data(i, NotificationCenter::SeverityRole).toInt(),
             static_cast<int>(Level::Critical));
    QCOMPARE(n.data(i, NotificationCenter::CategoryRole).toString(),
             QStringLiteral("transport"));
    QCOMPARE(n.data(i, NotificationCenter::TextRole).toString(),
             QStringLiteral("socket closed"));
    QVERIFY(n.data(i, NotificationCenter::AgeMsRole).toInt() >= 0);
    QVERIFY(n.data(i, NotificationCenter::StickyRole).toBool());

    // An out-of-range row answers rather than crashing: a QML index can go
    // stale the moment a card is dismissed.
    QVERIFY(!n.data(n.index(99, 0), NotificationCenter::TextRole).isValid());
    QVERIFY(!n.data(QModelIndex(), NotificationCenter::TextRole).isValid());
}

void TestNotificationCenter::infoExpiresAndErrorDoesNot()
{
    NotificationCenter n;
    n.setInfoLifetimeMs(500);
    n.post(Level::Info, Category::Auth, QStringLiteral("signed in"));
    n.post(Level::Error, Category::Auth, QStringLiteral("sign-in refused"));

    QCOMPARE(n.rowCount(), 2);
    QTest::qWait(1200);
    // The informational card is gone because it has been read or superseded.
    // The error is still there because nobody dismissed it.
    QCOMPARE(n.rowCount(), 1);
    QCOMPARE(n.data(n.index(0, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("sign-in refused"));

    // A warning is not expiring either: it is the same situation as an error
    // with less certainty, not less importance.
    n.post(Level::Warning, Category::Drops, QStringLiteral("dropped frames"));
    QTest::qWait(1200);
    QCOMPARE(n.rowCount(), 2);
}

void TestNotificationCenter::repeatingTheSameKeyDoesNotGrowTheList()
{
    NotificationCenter n;
    // A link that flaps must not fill the screen with the same sentence, and
    // must not push everything else out of view while doing it.
    for (int i = 0; i < 20; ++i) {
        n.postOnce(QStringLiteral("session-degraded"), Level::Warning, Category::Connection,
                   QStringLiteral("video only - control unreachable"));
    }
    QCOMPARE(n.rowCount(), 1);
    QCOMPARE(n.warningCount(), 1);
}

void TestNotificationCenter::repeatingTheSameKeyRefreshesAndMovesToTop()
{
    NotificationCenter n;
    n.postOnce(QStringLiteral("a"), Level::Warning, Category::Connection, QStringLiteral("alpha"));
    n.postOnce(QStringLiteral("b"), Level::Warning, Category::Connection, QStringLiteral("bravo"));
    QCOMPARE(n.data(n.index(0, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("bravo"));

    // A repeat of an older key goes back to the top, because it just happened
    // again and the more recent event is the relevant one.
    n.postOnce(QStringLiteral("a"), Level::Warning, Category::Connection, QStringLiteral("alpha"));
    QCOMPARE(n.rowCount(), 2);
    QCOMPARE(n.data(n.index(0, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(n.data(n.index(1, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("bravo"));

    // Escalating severity is carried through on the repeat.
    n.postOnce(QStringLiteral("a"), Level::Error, Category::Connection, QStringLiteral("alpha gone"));
    QCOMPARE(n.errorCount(), 1);
    QCOMPARE(n.warningCount(), 1);
}

void TestNotificationCenter::dismissRemovesOneRow()
{
    NotificationCenter n;
    n.post(Level::Warning, Category::Drops, QStringLiteral("one"));
    n.post(Level::Warning, Category::Drops, QStringLiteral("two"));

    n.dismiss(0);
    QCOMPARE(n.rowCount(), 1);
    QCOMPARE(n.data(n.index(0, 0), NotificationCenter::TextRole).toString(),
             QStringLiteral("one"));

    // Out-of-range dismissals are ignored rather than fatal, because the row
    // index a card holds can be stale by the time it is clicked.
    n.dismiss(99);
    n.dismiss(-1);
    QCOMPARE(n.rowCount(), 1);
}

void TestNotificationCenter::dismissAllClearsEverything()
{
    NotificationCenter n;
    n.post(Level::Warning, Category::Drops, QStringLiteral("one"));
    n.post(Level::Error, Category::Config, QStringLiteral("two"));

    n.acknowledgeAll();
    QCOMPARE(n.rowCount(), 0);
    QCOMPARE(n.unacknowledgedCount(), 0);
    QCOMPARE(n.errorCount(), 0);
}

void TestNotificationCenter::countsTrackSeverity()
{
    NotificationCenter n;
    n.post(Level::Debug, Category::Other, QStringLiteral("d"));
    n.post(Level::Info, Category::Auth, QStringLiteral("i"));
    n.post(Level::Warning, Category::Drops, QStringLiteral("w"));
    n.post(Level::Error, Category::Config, QStringLiteral("e"));
    n.post(Level::Critical, Category::Transport, QStringLiteral("c"));

    QCOMPARE(n.infoCount(), 2);
    QCOMPARE(n.warningCount(), 1);
    QCOMPARE(n.errorCount(), 2);
    // The badge counts what needs a human: warning and above. Of the five
    // posted, three qualify - warning, error and critical - and only the debug
    // and info lines are left to expire on their own.
    QCOMPARE(n.unacknowledgedCount(), 3);
    QCOMPARE(n.rowCount(), 5);
}

void TestNotificationCenter::stickyIsWarningAndAbove()
{
    NotificationCenter n;
    n.post(Level::Debug, Category::Other, QStringLiteral("d"));
    n.post(Level::Info, Category::Other, QStringLiteral("i"));
    n.post(Level::Warning, Category::Other, QStringLiteral("w"));
    n.post(Level::Error, Category::Other, QStringLiteral("e"));

    // Newest first, so the error is row 0 and the debug line is row 3. Reading
    // them in the other order would assert the opposite of the design. Critical
    // is included in the sticky set: it is simply a worse error.
    QVERIFY(n.data(n.index(0, 0), NotificationCenter::StickyRole).toBool());
    QVERIFY(n.data(n.index(1, 0), NotificationCenter::StickyRole).toBool());
    QVERIFY(!n.data(n.index(2, 0), NotificationCenter::StickyRole).toBool());
    QVERIFY(!n.data(n.index(3, 0), NotificationCenter::StickyRole).toBool());
}

void TestNotificationCenter::emptyTextIsNotPosted()
{
    NotificationCenter n;
    n.post(Level::Error, Category::Config, QString());
    n.postOnce(QStringLiteral("k"), Level::Error, Category::Config, QString());
    // A blank card is worse than no card: it occupies space and says nothing.
    QCOMPARE(n.rowCount(), 0);
}

void TestNotificationCenter::distinctKeysAccumulate()
{
    NotificationCenter n;
    n.postOnce(QStringLiteral("a"), Level::Info, Category::Auth, QStringLiteral("alpha"));
    n.postOnce(QStringLiteral("b"), Level::Info, Category::Config, QStringLiteral("bravo"));
    n.postOnce(QStringLiteral("c"), Level::Info, Category::Drops, QStringLiteral("charlie"));
    // Deduplication must be per condition, not a global single slot, or one
    // repeating fault would hide the others.
    QCOMPARE(n.rowCount(), 3);
}

QTEST_MAIN(TestNotificationCenter)
#include "tst_notificationcenter.moc"
