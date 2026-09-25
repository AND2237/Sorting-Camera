#include "DeviceStatus.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kPollIntervalMs = 1000;
constexpr int kRequestTimeoutMs = 3000;
} // namespace

class DeviceStatusWorker : public QObject
{
    Q_OBJECT

public:
    explicit DeviceStatusWorker(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~DeviceStatusWorker() override
    {
        abortReply();
        abortConfigReply();
    }

public slots:
    void startPolling(const QString &host, quint16 port)
    {
        m_url = QUrl(QStringLiteral("http://%1:%2/api/v1/status")
                         .arg(host)
                         .arg(port));
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setInterval(kPollIntervalMs);
            connect(m_timer, &QTimer::timeout, this, &DeviceStatusWorker::pollOnce);
        }
        m_timer->start();
        pollOnce();
    }

    void stopPolling()
    {
        if (m_timer) {
            m_timer->stop();
        }
        abortReply();
    }

    void setConfig(const QString &host, quint16 port, const QString &query)
    {
        m_cfgHost = host;
        m_cfgPort = port;
        m_cfgQuery = query;
        m_cfgRetries = 0;
        doConfig(host, port, query);
    }

signals:
    void statusUpdated(const QJsonObject &status);
    void onlineUpdated(bool online);
    void errorUpdated(const QString &errorString);
    void configFinished(bool ok, const QString &message);

private:
    void doConfig(const QString &host, quint16 port, const QString &query)
    {
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }
        abortConfigReply();

        const QUrl url(QStringLiteral("http://%1:%2/api/v1/config?%3")
                           .arg(host)
                           .arg(port)
                           .arg(query));
        m_configReply = m_nam->get(QNetworkRequest(url));
        QTimer::singleShot(kRequestTimeoutMs, m_configReply, &QNetworkReply::abort);
        connect(m_configReply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_configReply;
            m_configReply = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();

            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
                QString message;
                if (status == 409) {
                    if (m_cfgRetries < 1) {
                        ++m_cfgRetries;
                        qDebug() << "[config] 409 stale client, retrying in 1000 ms";
                        QTimer::singleShot(1000, this, [this]() {
                            if (!m_configReply) {
                                doConfig(m_cfgHost, m_cfgPort, m_cfgQuery);
                            }
                        });
                        return;
                    }
                    message = QStringLiteral("stream active: disconnect before config change");
                } else {
                    const QByteArray body = reply->readAll();
                    if (!body.isEmpty()) {
                        message = QString::fromUtf8(body).trimmed();
                    } else {
                        message = reply->errorString();
                    }
                }
                emit configFinished(false, message);
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isObject()) {
                emit statusUpdated(doc.object());
            }
            emit configFinished(true, QString());
        });
    }

private slots:
    void pollOnce()
    {
        if (m_reply) {
            return;
        }

        m_reply = m_nam->get(QNetworkRequest(m_url));
        QTimer::singleShot(kRequestTimeoutMs, m_reply, &QNetworkReply::abort);
        connect(m_reply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                if (m_online) {
                    qDebug() << "[status] offline:" << reply->errorString();
                    m_online = false;
                    emit onlineUpdated(false);
                }
                if (reply->error() != QNetworkReply::OperationCanceledError) {
                    emit errorUpdated(reply->errorString());
                }
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isObject()) {
                emit errorUpdated(QStringLiteral("invalid status JSON"));
                if (m_online) {
                    m_online = false;
                    emit onlineUpdated(false);
                }
                return;
            }

            m_status = doc.object();
            emit statusUpdated(m_status);
            if (!m_online) {
                qDebug() << "[status] online";
                m_online = true;
                emit onlineUpdated(true);
            }
            emit errorUpdated(QString());
        });
    }

    void abortReply()
    {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    void abortConfigReply()
    {
        QNetworkReply *reply = m_configReply;
        m_configReply = nullptr;
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

private:
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_reply = nullptr;
    QNetworkReply *m_configReply = nullptr;
    QUrl m_url;
    QJsonObject m_status;
    bool m_online = false;
    QString m_cfgHost;
    quint16 m_cfgPort = 0;
    QString m_cfgQuery;
    int m_cfgRetries = 0;
};

DeviceStatus::DeviceStatus(QObject *parent)
    : QObject(parent)
    , m_thread(new QThread(this))
    , m_worker(new DeviceStatusWorker)
{
    m_thread->setObjectName(QStringLiteral("device-status"));
    m_worker->moveToThread(m_thread);

    connect(m_worker, &DeviceStatusWorker::statusUpdated, this, [this](const QJsonObject &status) {
        m_status = status;
        emit statusChanged();
    });
    connect(m_worker, &DeviceStatusWorker::onlineUpdated, this, [this](bool online) {
        if (m_online != online) {
            m_online = online;
            emit onlineChanged();
        }
    });
    connect(m_worker, &DeviceStatusWorker::errorUpdated, this, [this](const QString &err) {
        if (m_errorString != err) {
            m_errorString = err;
            emit errorStringChanged();
        }
    });
    connect(m_worker, &DeviceStatusWorker::configFinished, this,
            [this](bool ok, const QString &message) {
                qDebug() << "[config] finished ok=" << ok << message;
                m_configBusy = false;
                emit configBusyChanged();
                const QString err = ok ? QString() : message;
                if (m_configError != err) {
                    m_configError = err;
                    emit configErrorChanged();
                }
            });

    m_thread->start();
}

DeviceStatus::~DeviceStatus()
{
    disconnect(m_worker, nullptr, this, nullptr);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stopPolling", Qt::BlockingQueuedConnection);
    }
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
    m_worker = nullptr;
}

bool DeviceStatus::isOnline() const
{
    return m_online;
}

QJsonObject DeviceStatus::status() const
{
    return m_status;
}

QString DeviceStatus::errorString() const
{
    return m_errorString;
}

QString DeviceStatus::configError() const
{
    return m_configError;
}

bool DeviceStatus::configBusy() const
{
    return m_configBusy;
}

void DeviceStatus::startPolling(const QString &host, quint16 port)
{
    if (host.trimmed().isEmpty()) {
        return;
    }
    m_host = host.trimmed();
    m_port = port;
    QMetaObject::invokeMethod(m_worker, "startPolling", Qt::QueuedConnection,
                              Q_ARG(QString, host.trimmed()), Q_ARG(quint16, port));
}

void DeviceStatus::stopPolling()
{
    QMetaObject::invokeMethod(m_worker, "stopPolling", Qt::QueuedConnection);
}

void DeviceStatus::setResolution(const QString &framesizeKey)
{
    if (framesizeKey.isEmpty()) {
        return;
    }
    setConfigQuery(QStringLiteral("framesize=%1").arg(framesizeKey));
}

void DeviceStatus::setQuality(int quality)
{
    setConfigQuery(QStringLiteral("quality=%1").arg(qBound(0, quality, 63)));
}

void DeviceStatus::setConfigQuery(const QString &query)
{
    if (m_host.isEmpty()) {
        if (m_configError != QStringLiteral("not connected")) {
            m_configError = QStringLiteral("not connected");
            emit configErrorChanged();
        }
        return;
    }
    if (m_configBusy) {
        return;
    }
    m_configBusy = true;
    emit configBusyChanged();
    if (!m_configError.isEmpty()) {
        m_configError.clear();
        emit configErrorChanged();
    }
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, m_host), Q_ARG(quint16, m_port),
                              Q_ARG(QString, query));
}

#include "DeviceStatus.moc"
