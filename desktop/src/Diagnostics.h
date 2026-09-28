#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class QTimer;

// Master Prompt 38. Structured logs, five levels, per-subsystem categories,
// and - the part that actually bites at 20 fps - rate-limited and statistical
// reporting instead of a line per frame.
//
// The facility installs a Qt message handler, so a plain qDebug()/qInfo() is
// captured with a category taken from whatever Diagnostics::scope() the caller
// is inside. That is deliberate: the alternative is rewriting every logging
// call in the pipeline, and a call site that forgets to be converted is an
// invisible hole in the diagnostics. Here the only thing a subsystem has to do
// is declare which one it is.
namespace Diagnostics {

Q_NAMESPACE

enum class Level { Debug = 0, Info, Warning, Error, Critical };

Q_ENUM_NS(Level)

// The subsystems section 38 names, plus the two Phase 6 additions. Order is
// the display order, so keep it stable.
enum class Category {
    Discovery = 0,
    Auth,
    Connection,
    Transport,
    Frames,
    Drops,
    Decode,
    Render,
    Recording,
    Config,
    Profile,
    Other,
    CategoryCount,
};
Q_ENUM_NS(Category)

QString levelName(Level level);
QString categoryName(Category category);
bool parseLevel(const QString &name, Level *out);

struct Entry
{
    qint64 timestampMs = 0;
    Level level = Level::Info;
    Category category = Category::Other;
    QString message;
    // Messages suppressed since the last line that got through at this
    // category. Never silently dropped: a rate limiter that hides how often
    // something happened makes a log useless for the one question it was asked.
    int suppressed = 0;
};

class Facility : public QObject
{
    Q_OBJECT

public:
    explicit Facility(QObject *parent = nullptr);
    ~Facility() override;

    // The process-wide instance. One log, one ring buffer, one set of limits.
    static Facility *instance();

    // Reachable from QML: the level combo in the diagnostics panel drives it.
    Q_INVOKABLE void setMinimumLevel(int level);
    Level minimumLevel() const;

    // Categories at or above this are held to a tighter budget, because they
    // are the ones that say a stream is unhealthy.
    void setRateLimit(Level level, int maxPerWindowMs, int maxCount);

    void log(Category category, Level level, const QString &message);

    // ---- statistical reporting for the high-rate paths ----
    // A counter that only reports when it changes, or on an interval. Used for
    // frame reception, drops and decode failures, where a line per frame would
    // be both useless and fatal to the frame rate it is describing.
    void countFrame(const QString &field, qint64 value);
    void countDrop(const QString &reason, qint64 value);
    QString statistics() const;
    void resetStatistics();

    const QVector<Entry> &entries() const;
    int entryCount() const;
    int countAtLeast(Level level) const;
    int countInCategory(Category category) const;
    // The most recent entries, newest last, for the diagnostics panel.
    Q_INVOKABLE QVariantList recentEntries(int max) const;
    Q_INVOKABLE QString statisticsText() const;
    Q_INVOKABLE QString levelNameAt(int index) const;
    Q_INVOKABLE QString categoryNameAt(int index) const;
    Q_INVOKABLE int errorCount() const;
    Q_INVOKABLE int warningCount() const;

    // Total messages dropped by the rate limiter since startup. Reported so a
    // truncated log is never mistaken for a quiet one.
    Q_INVOKABLE int suppressedTotal() const;

    void setMaxEntries(int max);
    int maxEntries() const { return m_maxEntries; }

signals:
    void entryAdded();
    void statisticsChanged();

private:
    void appendLocked(const Entry &entry);
    void timerTick();
    bool budgetAllows(Category category, Level level, qint64 now);

    QVector<Entry> m_entries;
    int m_maxEntries = 500;
    Level m_minimumLevel = Level::Debug;
    int m_suppressedTotal = 0;

    struct Limit
    {
        int windowMs = 5000;
        int maxCount = 5;
    };
    QVector<Limit> m_limits;
    struct Window
    {
        qint64 startedMs = 0;
        int used = 0;
        int pending = 0;
    };
    QVector<Window> m_windows;

    // A running counter for a high-rate quantity. min/max are kept so the
    // panel can show the spread of a frame interval or a frame size without
    // anyone having to remember to log it.
    struct Counter
    {
        QString name;
        qint64 last = 0;
        qint64 min = 0;
        qint64 max = 0;
        qint64 samples = 0;
        qint64 lastMs = 0;
    };
    QVector<Counter> m_counters;
    QTimer *m_timer = nullptr;
};

class Facility;

// Declares which subsystem the calling code is, for the lifetime of the
// object. Everything logged inside - including plain qDebug() from code that
// knows nothing about this - is filed under that category and passes through
// the rate limiter for its level.
//
// The alternative, converting each log call to a categorised helper, puts the
// burden on every call site and makes an unconverted one an invisible hole.
// One guard per subsystem cannot be forgotten by accident in the same way.
class Scope
{
public:
    explicit Scope(Category category);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    Category category() const { return m_previous; }

private:
    Category m_previous;
    bool m_previousScoped;
};

} // namespace Diagnostics
