#include "NotificationCenter.h"

NotificationCenter::NotificationCenter(QObject *parent) : QAbstractListModel(parent)
{
    m_sweep = new QTimer(this);
    // Frequent enough that an expiry is not visibly late, rare enough to be free.
    m_sweep->setInterval(500);
    connect(m_sweep, &QTimer::timeout, this, &NotificationCenter::sweepExpired);
    m_sweep->start();
}

int NotificationCenter::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_items.size();
}

QVariant NotificationCenter::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return {};
    }
    const Item &item = m_items.at(index.row());
    switch (role) {
    case SeverityRole:
        return static_cast<int>(item.level);
    case CategoryRole:
        return Diagnostics::categoryName(item.category);
    case TextRole:
        return item.text;
    case AgeMsRole:
        return QDateTime::currentMSecsSinceEpoch() - item.createdMs;
    case StickyRole:
        return isSticky(item.level);
    default:
        return {};
    }
}

QHash<int, QByteArray> NotificationCenter::roleNames() const
{
    return {
        {SeverityRole, "severity"},
        {CategoryRole, "category"},
        {TextRole, "text"},
        {AgeMsRole, "ageMs"},
        {StickyRole, "sticky"},
    };
}

bool NotificationCenter::isSticky(Diagnostics::Level level) const
{
    // Anything at warning or above waits to be dismissed by a person.
    return level >= Diagnostics::Level::Warning;
}

int NotificationCenter::indexOfKey(const QString &key) const
{
    // An empty key means "no key", not "the key that happens to be empty".
    // post() deliberately does not deduplicate, and several unconditional
    // posts in a row would otherwise all collapse onto the first item that
    // also happened to have no key.
    if (key.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).key == key) {
            return i;
        }
    }
    return -1;
}

void NotificationCenter::post(Diagnostics::Level level, Diagnostics::Category category,
                              const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    Item item;
    item.id = m_nextId++;
    item.createdMs = QDateTime::currentMSecsSinceEpoch();
    item.level = level;
    item.category = category;
    item.text = text;

    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(item);
    endInsertRows();

    emit countsChanged();
    emit posted(level, category, text);
}

void NotificationCenter::postOnce(const QString &key, Diagnostics::Level level,
                                  Diagnostics::Category category, const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    const int existing = indexOfKey(key);
    if (existing >= 0) {
        // Refresh in place and move to the top. A link that flaps every two
        // seconds must not fill the list with the same sentence, and must not
        // push everything else off the screen while doing it.
        m_items[existing].text = text;
        m_items[existing].createdMs = QDateTime::currentMSecsSinceEpoch();
        m_items[existing].level = level;
        const Item moved = m_items.takeAt(existing);
        beginInsertRows(QModelIndex(), 0, 0);
        m_items.prepend(moved);
        endInsertRows();
        emit countsChanged();
        emit posted(level, category, text);
        return;
    }
    Item item;
    item.id = m_nextId++;
    item.createdMs = QDateTime::currentMSecsSinceEpoch();
    item.level = level;
    item.category = category;
    item.text = text;
    item.key = key;

    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(item);
    endInsertRows();

    emit countsChanged();
    emit posted(level, category, text);
}

void NotificationCenter::dismiss(int row)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_items.remove(row);
    endRemoveRows();
    emit countsChanged();
}

void NotificationCenter::dismissAll()
{
    if (m_items.isEmpty()) {
        return;
    }
    beginResetModel();
    m_items.clear();
    endResetModel();
    emit countsChanged();
}

void NotificationCenter::acknowledgeAll()
{
    // Acknowledging is not the same as forgetting: sticky items are removed,
    // and the unacknowledged counter falls to zero so the badge can go.
    dismissAll();
}

void NotificationCenter::sweepExpired()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (isSticky(m_items.at(i).level)) {
            continue;
        }
        if (now - m_items.at(i).createdMs >= m_infoLifetimeMs) {
            beginRemoveRows(QModelIndex(), i, i);
            m_items.remove(i);
            endRemoveRows();
        }
    }
}

int NotificationCenter::errorCount() const
{
    int n = 0;
    for (const Item &i : m_items) {
        if (i.level == Diagnostics::Level::Error || i.level == Diagnostics::Level::Critical) {
            ++n;
        }
    }
    return n;
}

int NotificationCenter::warningCount() const
{
    int n = 0;
    for (const Item &i : m_items) {
        if (i.level == Diagnostics::Level::Warning) {
            ++n;
        }
    }
    return n;
}

int NotificationCenter::infoCount() const
{
    int n = 0;
    for (const Item &i : m_items) {
        if (i.level == Diagnostics::Level::Info || i.level == Diagnostics::Level::Debug) {
            ++n;
        }
    }
    return n;
}

int NotificationCenter::unacknowledgedCount() const
{
    int n = 0;
    for (const Item &i : m_items) {
        if (isSticky(i.level)) {
            ++n;
        }
    }
    return n;
}

int NotificationCenter::infoLifetimeMs() const
{
    return m_infoLifetimeMs;
}

void NotificationCenter::setInfoLifetimeMs(int ms)
{
    m_infoLifetimeMs = qMax(500, ms);
}

void NotificationCenter::refreshRow(int row)
{
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx);
}
