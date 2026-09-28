#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QTimer>
#include <QVector>

#include "Diagnostics.h"

// Master Prompt 26, "error / status notifications". The conditions worth
// telling someone about are already detected - the session state machine names
// eight states, the recorder reports failures, the control channel reports its
// own errors - but they were each shown in a different corner of the window, and
// the ones that mattered most were the ones with the least visible treatment.
//
// This is one place that collects them, in severity order, newest first, and
// gives each a lifetime. Information expires because a user who has read that
// the camera was discovered does not need to read it again in ten seconds.
// An error does not expire on its own, because nobody has acknowledged it yet.
class NotificationCenter : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int errorCount READ errorCount NOTIFY countsChanged)
    Q_PROPERTY(int warningCount READ warningCount NOTIFY countsChanged)
    Q_PROPERTY(int infoCount READ infoCount NOTIFY countsChanged)
    Q_PROPERTY(int unacknowledgedCount READ unacknowledgedCount NOTIFY countsChanged)

public:
    enum Roles { SeverityRole = Qt::UserRole + 1, CategoryRole, TextRole, AgeMsRole, StickyRole };
    Q_ENUM(Roles)

    explicit NotificationCenter(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int errorCount() const;
    int warningCount() const;
    int infoCount() const;
    int unacknowledgedCount() const;

    // Severity is taken from the Diagnostics level vocabulary so a condition
    // cannot be a warning in the log and an error on screen because two
    // enums drifted apart.
    void post(Diagnostics::Level level, Diagnostics::Category category, const QString &text);

    // The same condition twice in a row is not two notifications. Repeated
    // posts to an identical key refresh the existing entry and move it to the
    // top instead, so a flapping link cannot bury the rest of the list.
    void postOnce(const QString &key, Diagnostics::Level level, Diagnostics::Category category,
                  const QString &text);

    Q_INVOKABLE void dismiss(int row);
    Q_INVOKABLE void dismissAll();
    Q_INVOKABLE void acknowledgeAll();

    // How long an informational message stays. Errors and warnings are sticky
    // and ignore this.
    int infoLifetimeMs() const;
    void setInfoLifetimeMs(int ms);

signals:
    void countsChanged();
    void posted(Diagnostics::Level level, Diagnostics::Category category, const QString &text);

private:
    struct Item
    {
        qint64 id = 0;
        qint64 createdMs = 0;
        Diagnostics::Level level = Diagnostics::Level::Info;
        Diagnostics::Category category = Diagnostics::Category::Other;
        QString text;
        QString key;
    };

    void sweepExpired();
    bool isSticky(Diagnostics::Level level) const;
    int indexOfKey(const QString &key) const;
    void refreshRow(int row);

    QVector<Item> m_items;
    qint64 m_nextId = 1;
    int m_infoLifetimeMs = 8000;
    QTimer *m_sweep = nullptr;
};
