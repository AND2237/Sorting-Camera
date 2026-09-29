#include "DeviceStatus.h"

#include "AuthClient.h"
#include "CredentialStore.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kPollIntervalMs = 1000;
constexpr int kRequestTimeoutMs = 3000;
constexpr int kDeviceFreeWaitMs = 8000;
// A profile is five setters called back to back. They are coalesced into the
// one request /api/v1/config is designed to take rather than raced at the
// camera as five separate round trips.
constexpr int kConfigDebounceMs = 120;
// How long to wait for a status poll to land before sending anyway. The
// camera's own stream_clients count is what makes a config write legal, so
// guessing at it is what produced the 409.
constexpr int kConfigStatusWaitMs = 1500;
// How long to wait for a sign-in we asked for after a 401 before giving up on
// the write it is meant to unblock. Long enough for a challenge plus a login
// round trip against a camera that is also streaming, short enough that a
// slider drag does not look like it went nowhere.
constexpr int kAuthRetryWaitMs = 15000;
} // namespace

class DeviceStatusWorker : public QObject
{
    Q_OBJECT

public:
    explicit DeviceStatusWorker(QObject *parent = nullptr)
        : QObject(parent)
        , m_auth(new AuthClient(this))
    {
        connect(m_auth, &AuthClient::authenticatedChanged, this, [this]() {
            emit authChanged();
            if (m_auth->isAuthenticated()) {
                emit authRequiredChanged(false);
                pollOnce();
            }
        });
        connect(m_auth, &AuthClient::busyChanged, this, &DeviceStatusWorker::authChanged);
        connect(m_auth, &AuthClient::lastErrorChanged, this, &DeviceStatusWorker::authChanged);
        connect(m_auth, &AuthClient::settled, this, [this]() {
            if (!m_auth->isAuthenticated() && !m_auth->hasPassword()) {
                emit authRequiredChanged(true);
            }
            pollOnce();
            resumeWritesAfterSignIn();
        });
        // AuthClient::invalidate() aborts a sign-in attempt without settling it,
        // so a wait that trusts settled() alone can hold the panel busy for ever.
        // This is the bound on that wait.
        m_authRetryTimer = new QTimer(this);
        m_authRetryTimer->setSingleShot(true);
        m_authRetryTimer->setInterval(kAuthRetryWaitMs);
        connect(m_authRetryTimer, &QTimer::timeout, this, [this]() {
            failPendingWrites(QStringLiteral("session expired: sign-in did not finish"));
        });
    }

    ~DeviceStatusWorker() override
    {
        abortReply();
        abortConfigReply();
    }

public slots:
    void startPolling(const QString &host, quint16 port, const QString &password)
    {
        m_url = QUrl(QStringLiteral("http://%1:%2/api/v1/status")
                         .arg(host)
                         .arg(port));
        m_host = host;
        m_controlPort = port;
        m_auth->configure(host, port);
        m_auth->setPassword(password);
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setInterval(kPollIntervalMs);
            connect(m_timer, &QTimer::timeout, this, &DeviceStatusWorker::pollOnce);
        }
        m_timer->start();
        if (!m_auth->hasPassword()) {
            emit authRequiredChanged(true);
            pollOnce();
        } else if (!m_auth->isAuthenticated()) {
            m_auth->signIn();
        } else {
            pollOnce();
        }
    }

    void refreshStatus()
    {
        pollOnce();
    }

    void stopPolling()
    {
        if (m_timer) {
            m_timer->stop();
        }
        abortReply();
        abortCapabilitiesReply();
        abortSensorReply();
    }

    void submitPassword(const QString &password)
    {
        m_auth->setPassword(password);
        if (password.isEmpty()) {
            emit authRequiredChanged(true);
            return;
        }
        m_auth->signIn();
    }

    void forgetPassword()
    {
        m_auth->forgetPassword();
        emit authRequiredChanged(true);
    }

    void setConfig(const QString &host, quint16 port, const QString &query)
    {
        m_cfgHost = host;
        m_cfgPort = port;
        m_cfgQuery = query;
        m_cfgRetries = 0;
        // A newer write supersedes whatever the previous one was waiting to
        // replay; keeping both would send a value the user has already changed.
        m_cfgAuthRetryPending = false;
        if (m_authRetryTimer && !m_sensorAuthRetryPending) {
            m_authRetryTimer->stop();
        }
        doConfig(host, port, query);
    }

    void fetchCapabilities(const QString &host, quint16 port)
    {
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }
        abortCapabilitiesReply();

        const QUrl url(QStringLiteral("http://%1:%2/api/v1/capabilities").arg(host).arg(port));
        QNetworkRequest request(url);
        m_auth->applyAuthHeaderPublic(&request);
        m_capReply = m_nam->get(request);
        QTimer::singleShot(kRequestTimeoutMs, m_capReply, &QNetworkReply::abort);
        connect(m_capReply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_capReply;
            m_capReply = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                // Capabilities are a convenience, not a requirement: the view
                // still works without them, so this is not a hard failure.
                qDebug() << "[caps] unavailable:" << reply->errorString();
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isObject()) {
                emit capabilitiesUpdated(doc.object());
            }
        });
    }

    // Sensor writes are a plain POST of the changed control, kept separate
    // from setConfig so a slider cannot be mistaken for a stream-tearing
    // reconfiguration.
    void setSensor(const QString &host, quint16 port, const QString &body)
    {
        m_sensorHost = host;
        m_sensorPort = port;
        m_sensorBody = body;
        m_sensorAuthRetryPending = false;
        if (m_authRetryTimer && !m_cfgAuthRetryPending) {
            m_authRetryTimer->stop();
        }
        sendSensor(host, port, body);
    }

    void sendSensor(const QString &host, quint16 port, const QString &body)
    {
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }
        abortSensorReply();

        const QUrl url(QStringLiteral("http://%1:%2/api/v1/sensor").arg(host).arg(port));
        QNetworkRequest request(url);
        m_auth->applyAuthHeaderPublic(&request);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        m_sensorReply = m_nam->post(request, body.toUtf8());
        QTimer::singleShot(kRequestTimeoutMs, m_sensorReply, &QNetworkReply::abort);
        connect(m_sensorReply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_sensorReply;
            m_sensorReply = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
                if (status == 401) {
                    if (m_sensorAuthRetryPending || m_sensorBody.isEmpty()) {
                        failPendingWrites(
                            QStringLiteral("session expired: the camera rejected the control twice"));
                        return;
                    }
                    m_sensorAuthRetryPending = true;
                    armAuthRetry();
                    m_auth->signIn();
                    if (m_sensorAuthRetryPending && !m_auth->isBusy()) {
                        failPendingWrites(
                            QStringLiteral("session expired: could not start a new sign-in"));
                    }
                    return;
                }
                const QByteArray err = reply->readAll();
                emit sensorFinished(false, err.isEmpty() ? reply->errorString()
                                                          : QString::fromUtf8(err).trimmed());
                return;
            }
            emit sensorFinished(true, QString());
        });
    }

signals:
    void statusUpdated(const QJsonObject &status);
    void capabilitiesUpdated(const QJsonObject &capabilities);
    void sensorFinished(bool ok, const QString &message);
    void onlineUpdated(bool online);
    void errorUpdated(const QString &errorString);
    void configFinished(bool ok, const QString &message);
    void authChanged();
    void authRequiredChanged(bool required);

public:
    bool authenticated() const { return m_auth->isAuthenticated(); }
    bool authBusy() const { return m_auth->isBusy(); }
    QString authError() const { return m_auth->lastError(); }
    QString currentPassword() const { return m_auth->password(); }

private:
    // A write that ran into a 401 used to answer "session expired, signing in
    // again" and stop - wording that promised a retry nothing then performed.
    // The batch it belonged to had already been cleared on the GUI side, so the
    // change was gone (CP-6). These two decide what actually happens next.
    void resumeWritesAfterSignIn()
    {
        const bool cfgWaiting = m_cfgAuthRetryPending;
        const bool sensorWaiting = m_sensorAuthRetryPending;
        if (!cfgWaiting && !sensorWaiting) {
            return;
        }
        if (m_authRetryTimer) {
            m_authRetryTimer->stop();
        }

        if (cfgWaiting) {
            m_cfgAuthRetryPending = false;
            if (m_auth->isAuthenticated()) {
                qDebug() << "[config] session restored, sending the write that hit 401";
                m_cfgRetries = 0;
                doConfig(m_cfgHost, m_cfgPort, m_cfgQuery);
            } else {
                emit configFinished(false, signInFailure());
            }
        }
        if (sensorWaiting) {
            m_sensorAuthRetryPending = false;
            if (m_auth->isAuthenticated()) {
                qDebug() << "[sensor] session restored, sending the control that hit 401";
                sendSensor(m_sensorHost, m_sensorPort, m_sensorBody);
            } else {
                emit sensorFinished(false, signInFailure());
            }
        }
    }

    void failPendingWrites(const QString &reason)
    {
        if (m_cfgAuthRetryPending) {
            m_cfgAuthRetryPending = false;
            emit configFinished(false, reason);
        }
        if (m_sensorAuthRetryPending) {
            m_sensorAuthRetryPending = false;
            emit sensorFinished(false, reason);
        }
        if (m_authRetryTimer) {
            m_authRetryTimer->stop();
        }
    }

    void armAuthRetry()
    {
        if (m_authRetryTimer) {
            m_authRetryTimer->start();
        }
    }

    QString signInFailure() const
    {
        const QString err = m_auth->lastError();
        return err.isEmpty()
                   ? QStringLiteral("session expired: the camera did not accept the sign-in")
                   : QStringLiteral("session expired: %1").arg(err);
    }

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
        QNetworkRequest request(url);
        m_auth->applyAuthHeaderPublic(&request);
        m_configReply = m_nam->get(request);
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
                if (status == 401) {
                    if (m_cfgAuthRetryPending || m_cfgQuery.isEmpty()) {
                        // This was already the replay. The camera is not going
                        // to accept it, so stop rather than hold the panel busy.
                        failPendingWrites(
                            QStringLiteral("session expired: the camera rejected the write twice"));
                        return;
                    }
                    // The batch was cleared on the GUI side before the request
                    // went out, so m_cfgQuery is the only record of what the
                    // user asked for. Keep it, keep the panel busy, and send it
                    // once the new session is up (CP-6).
                    m_cfgAuthRetryPending = true;
                    armAuthRetry();
                    m_auth->signIn();
                    if (m_cfgAuthRetryPending && !m_auth->isBusy()) {
                        // signIn() declined without settling (re-auth cooldown),
                        // so no settled() is coming to resume this write.
                        failPendingWrites(
                            QStringLiteral("session expired: could not start a new sign-in"));
                    }
                    return;
                }
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

        if (m_auth->hasPassword() && !m_auth->isAuthenticated()) {
            m_auth->signIn();
            return;
        }

        QNetworkRequest request(m_url);
        m_auth->applyAuthHeaderPublic(&request);
        m_reply = m_nam->get(request);
        QTimer::singleShot(kRequestTimeoutMs, m_reply, &QNetworkReply::abort);
        connect(m_reply, &QNetworkReply::finished, this, [this]() {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            if (!reply) {
                return;
            }
            reply->deleteLater();

            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401) {
                if (m_online) {
                    m_online = false;
                    emit onlineUpdated(false);
                }
                emit authRequiredChanged(true);
                m_auth->signIn();
                return;
            }

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

    void abortCapabilitiesReply()
    {
        QNetworkReply *reply = m_capReply;
        m_capReply = nullptr;
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    void abortSensorReply()
    {
        QNetworkReply *reply = m_sensorReply;
        m_sensorReply = nullptr;
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

private:
    AuthClient *m_auth = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_timer = nullptr;
    QNetworkReply *m_reply = nullptr;
    QNetworkReply *m_configReply = nullptr;
    QNetworkReply *m_capReply = nullptr;
    QNetworkReply *m_sensorReply = nullptr;
    QUrl m_url;
    QJsonObject m_status;
    bool m_online = false;
    QString m_host;
    quint16 m_controlPort = 80;
    QString m_cfgHost;
    quint16 m_cfgPort = 0;
    QString m_cfgQuery;
    int m_cfgRetries = 0;
    // Set when a write hit 401 and is waiting for the sign-in it triggered;
    // cleared by the replay, by the bound in m_authRetryTimer, or by a newer
    // write. While it is set no configFinished has been emitted, so the panel
    // stays busy on purpose.
    bool m_cfgAuthRetryPending = false;
    QString m_sensorHost;
    quint16 m_sensorPort = 0;
    QString m_sensorBody;
    bool m_sensorAuthRetryPending = false;
    QTimer *m_authRetryTimer = nullptr;
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
        const QString reportedId = status.value(QStringLiteral("device_id")).toString();
        if (!reportedId.isEmpty() && reportedId != m_deviceId) {
            m_deviceId = reportedId;
            if (m_credentialStored) {
                CredentialStore::movePassword(hostKey(), credentialKey());
            }
        }
        refreshAuthState();
        requestCapabilities();
        // Fresh enough to decide whether a config write is legal.
        m_statusFresh = true;
        trySendPendingConfig();
    });
    connect(m_worker, &DeviceStatusWorker::onlineUpdated, this, [this](bool online) {
        if (m_online != online) {
            m_online = online;
            emit onlineChanged();
        }
        // "not connected" stops being true the moment this answer arrives.
        // Leaving it standing after the camera replies is how a card outlives
        // the fault it describes, and nobody clears it because nobody is
        // watching the screen for it.
        if (online && m_configError == QStringLiteral("not connected")) {
            m_configError.clear();
            emit configErrorChanged();
        }
    });
    connect(m_worker, &DeviceStatusWorker::authChanged, this, [this]() {
        refreshAuthState();
    });
    connect(m_worker, &DeviceStatusWorker::authRequiredChanged, this, [this](bool required) {
        if (m_authRequired != required) {
            m_authRequired = required;
            emit authStateChanged();
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
                m_configInFlight = false;
                // Setters that arrived while this request was in flight form
                // the next batch; the panel stays busy until they are out too,
                // so nothing reads a half-applied profile.
                const bool busyNow = !m_pendingKv.isEmpty();
                if (m_configBusy != busyNow) {
                    m_configBusy = busyNow;
                    emit configBusyChanged();
                }
                const QString err = ok ? QString() : message;
                if (m_configError != err) {
                    m_configError = err;
                    emit configErrorChanged();
                }
                if (busyNow) {
                    trySendPendingConfig();
                }
            });

    connect(m_worker, &DeviceStatusWorker::capabilitiesUpdated, this,
            [this](const QJsonObject &caps) {
                if (caps == m_capabilities) {
                    return;
                }
                m_capabilities = caps;
                emit capabilitiesChanged();
            });
    connect(m_worker, &DeviceStatusWorker::sensorFinished, this,
            [this](bool ok, const QString &message) {
                m_sensorBusy = false;
                const QString err = ok ? QString() : message;
                if (m_sensorError != err) {
                    m_sensorError = err;
                }
                emit sensorStateChanged();
                if (ok) {
                    qDebug() << "[sensor] control applied";
                    QMetaObject::invokeMethod(m_worker, "refreshStatus",
                                              Qt::QueuedConnection);
                } else {
                    qDebug() << "[sensor] control rejected:" << message;
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

bool DeviceStatus::isPolling() const
{
    return m_polling;
}

QJsonObject DeviceStatus::status() const
{
    return m_status;
}

QJsonObject DeviceStatus::capabilities() const
{
    return m_capabilities;
}

bool DeviceStatus::isSensorBusy() const
{
    return m_sensorBusy;
}

QString DeviceStatus::sensorError() const
{
    return m_sensorError;
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

bool DeviceStatus::isAuthRequired() const
{
    return m_authRequired;
}

bool DeviceStatus::isAuthenticated() const
{
    return m_authenticated;
}

bool DeviceStatus::isAuthBusy() const
{
    return m_authBusy;
}

QString DeviceStatus::authError() const
{
    return m_authError;
}

bool DeviceStatus::isCredentialStored() const
{
    return m_credentialStored;
}

bool DeviceStatus::credentialStorageAvailable() const
{
    return CredentialStore::isPersistent();
}

QString DeviceStatus::hostKey() const
{
    return QStringLiteral("%1:%2").arg(m_host).arg(m_port);
}

QString DeviceStatus::credentialKey() const
{
    return m_deviceId.isEmpty() ? hostKey() : m_deviceId;
}

void DeviceStatus::refreshAuthState()
{
    const bool authenticated = m_worker->authenticated();
    const bool busy = m_worker->authBusy();
    const QString error = m_worker->authError();
    const bool stored = CredentialStore::hasPassword(credentialKey())
                        || CredentialStore::hasPassword(hostKey());
    if (m_authenticated != authenticated || m_authBusy != busy || m_authError != error
        || m_credentialStored != stored) {
        m_authenticated = authenticated;
        m_authBusy = busy;
        m_authError = error;
        m_credentialStored = stored;
        emit authStateChanged();
    }
}

void DeviceStatus::signIn(const QString &password, bool remember)
{
    if (remember && CredentialStore::isPersistent()) {
        CredentialStore::savePassword(credentialKey(), password);
    } else if (!remember) {
        CredentialStore::clearPassword(credentialKey());
        CredentialStore::clearPassword(hostKey());
    }
    refreshAuthState();
    QMetaObject::invokeMethod(m_worker, "submitPassword", Qt::QueuedConnection,
                              Q_ARG(QString, password));
}

void DeviceStatus::forgetCredential()
{
    CredentialStore::clearPassword(credentialKey());
    CredentialStore::clearPassword(hostKey());
    refreshAuthState();
    QMetaObject::invokeMethod(m_worker, "forgetPassword", Qt::QueuedConnection);
}

void DeviceStatus::retrySignIn()
{
    QMetaObject::invokeMethod(m_worker, "submitPassword", Qt::QueuedConnection,
                              Q_ARG(QString, m_worker->currentPassword()));
}

void DeviceStatus::startPolling(const QString &host, quint16 port)
{
    const QString trimmed = host.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    // Selecting the camera again must not look like a different camera.
    // Capabilities describe one device, and dropping them on every connect
    // emptied the controls panel for as long as it took to fetch them back -
    // which is the moment the panel is most likely to be on screen.
    const bool sameDevice = trimmed == m_host && port == m_port;
    m_host = trimmed;
    m_port = port;
    if (!sameDevice) {
        m_deviceId = QString();
        // Capabilities describe one camera. Carrying them across a switch to a
        // different one would offer controls the new device may not have.
        if (!m_capabilities.isEmpty()) {
            m_capabilities = QJsonObject();
            emit capabilitiesChanged();
        }
        m_sensorError.clear();
    }
    if (!m_polling) {
        m_polling = true;
        emit pollingChanged();
    }

    QString password = CredentialStore::loadPassword(hostKey());
    if (password.isEmpty() && m_credentialStored) {
        password = CredentialStore::loadPassword(m_deviceId);
    }
    refreshAuthState();

    QMetaObject::invokeMethod(m_worker, "startPolling", Qt::QueuedConnection,
                              Q_ARG(QString, trimmed), Q_ARG(quint16, port),
                              Q_ARG(QString, password));
}

void DeviceStatus::stopPolling()
{
    if (m_polling) {
        m_polling = false;
        emit pollingChanged();
    }
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

void DeviceStatus::setFrameBufferCount(int fbCount)
{
    setConfigQuery(QStringLiteral("fb_count=%1").arg(qBound(1, fbCount, 3)));
}

void DeviceStatus::setGrabMode(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m != QStringLiteral("latest") && m != QStringLiteral("cont")) {
        return;
    }
    setConfigQuery(QStringLiteral("grab=%1").arg(m));
}

void DeviceStatus::setXclk(int mhz)
{
    if (mhz < 6 || mhz > 27) {
        return;
    }
    setConfigQuery(QStringLiteral("xclk=%1").arg(mhz));
}

void DeviceStatus::requestCameraRecovery()
{
    const QString fs = m_status.value(QStringLiteral("resolution")).toString();
    static const QStringList keys = {
        QStringLiteral("qqvga"), QStringLiteral("qvga"), QStringLiteral("vga"),
        QStringLiteral("svga"), QStringLiteral("xga"), QStringLiteral("hd"),
        QStringLiteral("sxga"), QStringLiteral("uxga")};
    const QMap<QString, QString> bySize = {
        {QStringLiteral("160x120"), keys.value(0)}, {QStringLiteral("320x240"), keys.value(1)},
        {QStringLiteral("640x480"), keys.value(2)}, {QStringLiteral("800x600"), keys.value(3)},
        {QStringLiteral("1024x768"), keys.value(4)}, {QStringLiteral("1280x720"), keys.value(5)},
        {QStringLiteral("1280x1024"), keys.value(6)}, {QStringLiteral("1600x1200"), keys.value(7)}};
    QString key = bySize.value(fs, QString());
    if (key.isEmpty()) {
        key = QStringLiteral("hd");
    }
    qDebug() << "[config] requesting camera re-init at" << key;
    setConfigQuery(QStringLiteral("framesize=%1").arg(key));
}

// Capabilities are requested once the camera has answered a status poll, which
// only happens after authentication has succeeded - asking earlier would earn
// a 401 and never be retried. The 5 s floor stops a firmware that lacks the
// endpoint from being polled for it on every single status tick.
void DeviceStatus::requestCapabilities()
{
    if (!m_polling || m_host.isEmpty() || !m_capabilities.isEmpty()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_lastCapAttempt.isValid() || m_lastCapAttempt.secsTo(now) >= 5) {
        m_lastCapAttempt = now;
        QMetaObject::invokeMethod(m_worker, "fetchCapabilities", Qt::QueuedConnection,
                                  Q_ARG(QString, m_host), Q_ARG(quint16, m_port));
    }
}

void DeviceStatus::setSensorControl(const QString &name, int value)
{
    if (m_host.isEmpty() || name.isEmpty() || m_sensorBusy) {
        return;
    }
    // Only names the firmware advertises are sent: a typo reaching the camera
    // would be rejected, but a plausible-looking one would also be silently
    // accepted as an unknown key depending on firmware version.
    const QJsonObject ctrl =
        m_capabilities.value(QStringLiteral("controls")).toObject();
    if (!ctrl.contains(name)) {
        m_sensorError = QStringLiteral("unknown control: %1").arg(name);
        emit sensorStateChanged();
        return;
    }

    m_sensorBusy = true;
    m_sensorError.clear();
    emit sensorStateChanged();

    QJsonObject body;
    body.insert(name, value);
    QMetaObject::invokeMethod(
        m_worker, "setSensor", Qt::QueuedConnection, Q_ARG(QString, m_host),
        Q_ARG(quint16, m_port),
        Q_ARG(QString, QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact))));
}

void DeviceStatus::resetSensorControls()
{
    if (m_host.isEmpty() || m_sensorBusy) {
        return;
    }
    const QJsonObject ctrl =
        m_capabilities.value(QStringLiteral("controls")).toObject();
    if (ctrl.isEmpty()) {
        return;
    }
    QJsonObject body;
    for (auto it = ctrl.constBegin(); it != ctrl.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (!entry.value(QStringLiteral("supported")).toBool()) {
            continue;
        }
        body.insert(it.key(), entry.value(QStringLiteral("default")).toInt());
    }
    if (body.isEmpty()) {
        return;
    }

    m_sensorBusy = true;
    m_sensorError.clear();
    emit sensorStateChanged();
    qDebug() << "[sensor] restoring defaults for" << body.size() << "control(s)";
    QMetaObject::invokeMethod(
        m_worker, "setSensor", Qt::QueuedConnection, Q_ARG(QString, m_host),
        Q_ARG(quint16, m_port),
        Q_ARG(QString, QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact))));
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
    // Every setter adds to one batch instead of replacing it. The previous
    // version kept a single slot and returned early whenever a write was
    // already pending, so a profile - five setters invoked in the same tick -
    // sent the first and discarded the other four without a word. The camera
    // kept the configuration it already had, the profile selector appeared to
    // do nothing, and the four lost writes never reached a log either.
    bool added = false;
    const QStringList pairs = query.split(QLatin1Char('&'));
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        m_pendingKv.insert(pair.left(eq), pair.mid(eq + 1));
        added = true;
    }
    if (!added) {
        return;
    }

    if (!m_configBusy) {
        m_configBusy = true;
        emit configBusyChanged();
    }
    if (!m_configError.isEmpty()) {
        m_configError.clear();
        emit configErrorChanged();
    }

    m_statusFresh = false;
    m_waitDeadline = QDateTime::currentDateTime().addSecs(kDeviceFreeWaitMs);
    m_statusWaitDeadline = QDateTime::currentDateTime().addMSecs(kConfigStatusWaitMs);
    // Queued, not blocking. BlockingQueuedConnection stopped the GUI thread
    // until the worker picked the call up, and the worker is the thread that
    // talks to the camera - so the stall was longest precisely when the
    // network was busiest. The status update it asks for arrives as a signal
    // anyway, and that signal is what resumes the send.
    QMetaObject::invokeMethod(m_worker, "refreshStatus", Qt::QueuedConnection);

    const int gen = ++m_cfgDebounceGen;
    QTimer::singleShot(kConfigDebounceMs, this, [this, gen]() {
        if (gen == m_cfgDebounceGen) {
            trySendPendingConfig();
        }
    });
}

void DeviceStatus::trySendPendingConfig()
{
    if (m_pendingKv.isEmpty() || m_configInFlight) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_statusFresh && now < m_statusWaitDeadline) {
        QTimer::singleShot(100, this, [this]() { trySendPendingConfig(); });
        return;
    }
    if (!m_statusFresh) {
        qDebug("[config] no fresh status within %d ms, sending anyway", kConfigStatusWaitMs);
    }
    const int clients = m_status.value(QStringLiteral("stream_clients")).toInt(0);
    if (clients > 0) {
        if (now < m_waitDeadline) {
            QTimer::singleShot(250, this, [this]() { trySendPendingConfig(); });
            return;
        }
        qDebug("[config] device still reports %d stream client(s), sending anyway", clients);
    }

    QStringList parts;
    for (auto it = m_pendingKv.constBegin(); it != m_pendingKv.constEnd(); ++it) {
        parts << it.key() + QLatin1Char('=') + it.value();
    }
    m_pendingKv.clear();
    m_configInFlight = true;
    QMetaObject::invokeMethod(m_worker, "setConfig", Qt::QueuedConnection,
                              Q_ARG(QString, m_host), Q_ARG(quint16, m_port),
                              Q_ARG(QString, parts.join(QLatin1Char('&'))));
}

#include "DeviceStatus.moc"
