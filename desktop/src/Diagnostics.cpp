#include "Diagnostics.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QVariantList>

namespace Diagnostics {

namespace {

QMutex *g_mutex = nullptr;
Facility *g_instance = nullptr;
QtMessageHandler g_previousHandler = nullptr;
bool g_ownsHandler = false;

// The category applies to whatever runs inside a scope, so subsystems
// annotate themselves once instead of at every log call.
thread_local Category g_currentCategory = Category::Other;
thread_local bool g_inScope = false;

Level levelFromQtMessageType(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return Level::Debug;
    case QtInfoMsg:
        return Level::Info;
    case QtWarningMsg:
        return Level::Warning;
    case QtCriticalMsg:
    case QtFatalMsg:
        return Level::Critical;
    default:
        return Level::Info;
    }
}

QMutex *mutex()
{
    if (!g_mutex) {
        g_mutex = new QMutex;
    }
    return g_mutex;
}

void emitHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    const Level level = levelFromQtMessageType(type);
    // The category is per thread and defaults to Other, so a log from a thread
    // that never declared itself is still recorded - just not attributed.
    const Category category = g_inScope ? g_currentCategory : Category::Other;
    if (g_instance) {
        g_instance->log(category, level, msg);
    }
    if (g_previousHandler) {
        g_previousHandler(type, context, msg);
    } else if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace

// Scoped category. Everything logged while this is alive belongs to the given
// subsystem, including plain qDebug() calls made by code that has no idea any
// of this exists.
Scope::Scope(Category category) : m_previous(g_currentCategory), m_previousScoped(g_inScope)
{
    g_currentCategory = category;
    g_inScope = true;
}

Scope::~Scope()
{
    g_currentCategory = m_previous;
    g_inScope = m_previousScoped;
}

QString levelName(Level level)
{
    switch (level) {
    case Level::Debug:
        return QStringLiteral("DEBUG");
    case Level::Info:
        return QStringLiteral("INFO");
    case Level::Warning:
        return QStringLiteral("WARNING");
    case Level::Error:
        return QStringLiteral("ERROR");
    case Level::Critical:
        return QStringLiteral("CRITICAL");
    }
    return QStringLiteral("INFO");
}

QString categoryName(Category category)
{
    switch (category) {
    case Category::Discovery:
        return QStringLiteral("discovery");
    case Category::Auth:
        return QStringLiteral("auth");
    case Category::Connection:
        return QStringLiteral("connection");
    case Category::Transport:
        return QStringLiteral("transport");
    case Category::Frames:
        return QStringLiteral("frames");
    case Category::Drops:
        return QStringLiteral("drops");
    case Category::Decode:
        return QStringLiteral("decode");
    case Category::Render:
        return QStringLiteral("render");
    case Category::Recording:
        return QStringLiteral("recording");
    case Category::Config:
        return QStringLiteral("config");
    case Category::Profile:
        return QStringLiteral("profile");
    case Category::Other:
        return QStringLiteral("other");
    case Category::CategoryCount:
        break;
    }
    return QStringLiteral("other");
}

bool parseLevel(const QString &name, Level *out)
{
    const QString n = name.trimmed().toUpper();
    for (int i = 0; i <= static_cast<int>(Level::Critical); ++i) {
        const Level candidate = static_cast<Level>(i);
        if (levelName(candidate) == n) {
            if (out) {
                *out = candidate;
            }
            return true;
        }
    }
    return false;
}

Facility::Facility(QObject *parent) : QObject(parent)
{
    // Defaults: chatty levels are tightly budgeted because a stream that is
    // healthy does not need them and a stream that is broken does not need
    // thousands. Critical is never limited - if it fired once, it matters.
    m_limits = QVector<Limit>(static_cast<int>(Level::Critical) + 1);
    m_limits[static_cast<int>(Level::Debug)].maxCount = 5;
    m_limits[static_cast<int>(Level::Info)].maxCount = 8;
    m_limits[static_cast<int>(Level::Warning)].maxCount = 8;
    m_limits[static_cast<int>(Level::Error)].maxCount = 30;
    m_limits[static_cast<int>(Level::Critical)].maxCount = 0; // 0 = unlimited

    m_windows = QVector<Window>(m_limits.size());

    // The handler and the instance pointer are process-wide, so they are owned
    // by whichever Facility comes first and released only by that one. A second
    // Facility - which the tests create freely - must not take the handler over,
    // or destroying it would leave a handler installed that dispatches to a
    // freed object.
    if (!g_instance) {
        g_instance = this;
        g_ownsHandler = true;
        g_previousHandler = qInstallMessageHandler(emitHandler);
    }

    // Periodic flush of the statistical counters, so a long stream reports
    // aggregate numbers on an interval instead of per frame.
    m_timer = new QTimer(this);
    m_timer->setInterval(5000);
    connect(m_timer, &QTimer::timeout, this, &Facility::timerTick);
    m_timer->start();
}

Facility::~Facility()
{
    if (m_timer) {
        m_timer->stop();
    }
    if (g_instance == this) {
        g_instance = nullptr;
        if (g_ownsHandler) {
            qInstallMessageHandler(g_previousHandler);
            g_ownsHandler = false;
        }
    }
}

Facility *Facility::instance()
{
    return g_instance;
}

void Facility::setMinimumLevel(Level level)
{
    m_minimumLevel = level;
}

Level Facility::minimumLevel() const
{
    return m_minimumLevel;
}

void Facility::setRateLimit(Level level, int maxPerWindowMs, int maxCount)
{
    if (level < Level::Debug || level > Level::Critical) {
        return;
    }
    m_limits[static_cast<int>(level)].windowMs = qMax(1, maxPerWindowMs);
    m_limits[static_cast<int>(level)].maxCount = qMax(0, maxCount);
    // Only the spending window is reset. The pending suppression count is
    // deliberately preserved: a limit changed at runtime must not erase the
    // record of what the previous limit swallowed, or turning the level up
    // would quietly hide the very burst the user turned it up to see.
    m_windows[static_cast<int>(level)].startedMs = 0;
    m_windows[static_cast<int>(level)].used = 0;
}

bool Facility::budgetAllows(Category, Level level, qint64 now)
{
    const int li = static_cast<int>(level);
    Limit &limit = m_limits[li];
    Window &window = m_windows[li];
    if (limit.maxCount == 0) {
        return true; // unlimited
    }
    if (window.startedMs == 0 || now - window.startedMs >= limit.windowMs) {
        window.startedMs = now;
        window.used = 0;
    }
    if (window.used < limit.maxCount) {
        ++window.used;
        return true;
    }
    return false;
}

void Facility::appendLocked(const Entry &entry)
{
    m_entries.append(entry);
    while (m_entries.size() > m_maxEntries) {
        m_entries.removeFirst();
    }
}

void Facility::log(Category category, Level level, const QString &message)
{
    if (level < m_minimumLevel) {
        return;
    }

    QMutexLocker locker(mutex());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    int suppressed = 0;
    if (!budgetAllows(category, level, now)) {
        // Counted, not thrown away. The next line that gets through carries how
        // many were held back, so a rate-limited log still answers "how often
        // does this happen" without answering it 20 times a second.
        const int li = static_cast<int>(level);
        ++m_windows[li].pending;
        ++m_suppressedTotal;
        return;
    }
    {
        const int li = static_cast<int>(level);
        suppressed = m_windows[li].pending;
        m_windows[li].pending = 0;
    }

    Entry e;
    e.timestampMs = now;
    e.level = level;
    e.category = category;
    e.message = message;
    e.suppressed = suppressed;
    appendLocked(e);
    locker.unlock();

    emit entryAdded();
}

void Facility::countFrame(const QString &field, qint64 value)
{
    {
        QMutexLocker locker(mutex());
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (Counter &c : m_counters) {
            if (c.name != field) {
                continue;
            }
            c.last = value;
            c.lastMs = now;
            c.samples += 1;
            if (c.samples == 1) {
                c.min = value;
                c.max = value;
            } else {
                c.min = qMin(c.min, value);
                c.max = qMax(c.max, value);
            }
            return;
        }
        Counter c;
        c.name = field;
        c.last = value;
        c.min = value;
        c.max = value;
        c.samples = 1;
        c.lastMs = now;
        m_counters.append(c);
    }
    emit statisticsChanged();
}

void Facility::countDrop(const QString &reason, qint64 value)
{
    countFrame(QStringLiteral("drop.%1").arg(reason), value);
}

void Facility::timerTick()
{
    if (m_counters.isEmpty()) {
        return;
    }
    emit statisticsChanged();
}

QString Facility::statistics() const
{
    QMutexLocker locker(mutex());
    QStringList out;
    for (const Counter &c : m_counters) {
        if (c.samples <= 1) {
            out << QStringLiteral("%1=%2").arg(c.name).arg(c.last);
        } else {
            out << QStringLiteral("%1=%2 (%3..%4 over %5)")
                       .arg(c.name)
                       .arg(c.last)
                       .arg(c.min)
                       .arg(c.max)
                       .arg(c.samples);
        }
    }
    return out.join(QStringLiteral("  "));
}

void Facility::resetStatistics()
{
    {
        QMutexLocker locker(mutex());
        m_counters.clear();
    }
    emit statisticsChanged();
}

const QVector<Entry> &Facility::entries() const
{
    return m_entries;
}

int Facility::entryCount() const
{
    return m_entries.size();
}

int Facility::countAtLeast(Level level) const
{
    int n = 0;
    for (const Entry &e : m_entries) {
        if (e.level >= level) {
            ++n;
        }
    }
    return n;
}

int Facility::countInCategory(Category category) const
{
    int n = 0;
    for (const Entry &e : m_entries) {
        if (e.category == category) {
            ++n;
        }
    }
    return n;
}

QVariantList Facility::recentEntries(int max) const
{
    QMutexLocker locker(mutex());
    QVariantList out;
    const int start = qMax(0, m_entries.size() - qMax(1, max));
    for (int i = start; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        QString text = e.message;
        if (e.suppressed > 0) {
            text += QStringLiteral("  [+%1 suppressed]").arg(e.suppressed);
        }
        // Built up explicitly and wrapped in a QVariant before being appended.
        // Both details matter: a QVariantList holding one QString converts to
        // a QStringList, and appending a bare QVariantList to a QVariantList
        // flattens it into the outer list instead of storing it as a row, so
        // every accessor would see the row's individual fields.
        QVariantList row;
        row << QVariant::fromValue(static_cast<int>(e.level)) << QVariant(levelName(e.level))
            << QVariant(categoryName(e.category)) << QVariant(text);
        out.append(QVariant(row));
    }
    return out;
}

QString Facility::statisticsText() const
{
    return statistics();
}

QString Facility::levelNameAt(int index) const
{
    if (index < 0 || index > static_cast<int>(Level::Critical)) {
        return QString();
    }
    return levelName(static_cast<Level>(index));
}

QString Facility::categoryNameAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(Category::CategoryCount)) {
        return QString();
    }
    return categoryName(static_cast<Category>(index));
}

int Facility::errorCount() const
{
    return countAtLeast(Level::Error);
}

int Facility::warningCount() const
{
    int n = 0;
    for (const Entry &e : m_entries) {
        if (e.level == Level::Warning) {
            ++n;
        }
    }
    return n;
}

int Facility::suppressedTotal() const
{
    return m_suppressedTotal;
}

void Facility::setMaxEntries(int max)
{
    m_maxEntries = qMax(1, max);
    while (m_entries.size() > m_maxEntries) {
        m_entries.removeFirst();
    }
}

} // namespace Diagnostics
