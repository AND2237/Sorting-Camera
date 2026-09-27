#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QDateTime;
class QThread;
class AuthClient;

class DeviceStatusWorker;

class DeviceStatus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool online READ isOnline NOTIFY onlineChanged)
    Q_PROPERTY(bool polling READ isPolling NOTIFY pollingChanged)
    Q_PROPERTY(QJsonObject status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QString configError READ configError NOTIFY configErrorChanged)
    Q_PROPERTY(bool configBusy READ configBusy NOTIFY configBusyChanged)
    Q_PROPERTY(bool authRequired READ isAuthRequired NOTIFY authStateChanged)
    Q_PROPERTY(bool authenticated READ isAuthenticated NOTIFY authStateChanged)
    Q_PROPERTY(bool authBusy READ isAuthBusy NOTIFY authStateChanged)
    Q_PROPERTY(QString authError READ authError NOTIFY authStateChanged)
    Q_PROPERTY(bool credentialStored READ isCredentialStored NOTIFY authStateChanged)
    Q_PROPERTY(bool credentialStorageAvailable READ credentialStorageAvailable NOTIFY authStateChanged)

public:
    explicit DeviceStatus(QObject *parent = nullptr);
    ~DeviceStatus() override;

    bool isOnline() const;
    bool isPolling() const;
    QJsonObject status() const;
    QString errorString() const;
    QString configError() const;
    bool configBusy() const;
    bool isAuthRequired() const;
    bool isAuthenticated() const;
    bool isAuthBusy() const;
    QString authError() const;
    bool isCredentialStored() const;
    bool credentialStorageAvailable() const;

    Q_INVOKABLE void startPolling(const QString &host, quint16 port);
    Q_INVOKABLE void stopPolling();
    Q_INVOKABLE void setResolution(const QString &framesizeKey);
    Q_INVOKABLE void setQuality(int quality);
    Q_INVOKABLE void requestCameraRecovery();
    Q_INVOKABLE void setFrameBufferCount(int fbCount);
    Q_INVOKABLE void setGrabMode(const QString &mode);
    Q_INVOKABLE void setXclk(int mhz);
    Q_INVOKABLE void signIn(const QString &password, bool remember);
    Q_INVOKABLE void forgetCredential();
    Q_INVOKABLE void retrySignIn();

signals:
    void onlineChanged();
    void pollingChanged();
    void statusChanged();
    void errorStringChanged();
    void configErrorChanged();
    void configBusyChanged();
    void authStateChanged();

private:
    void setConfigQuery(const QString &query);
    void trySendPendingConfig();
    void refreshAuthState();
    QString hostKey() const;
    QString credentialKey() const;

    QThread *m_thread;
    DeviceStatusWorker *m_worker;
    bool m_online = false;
    bool m_polling = false;
    QJsonObject m_status;
    QString m_errorString;
    QString m_configError;
    bool m_configBusy = false;
    bool m_authRequired = false;
    bool m_authenticated = false;
    bool m_authBusy = false;
    bool m_credentialStored = false;
    QString m_authError;
    QString m_host;
    quint16 m_port = 0;
    QString m_deviceId;
    QString m_pendingQuery;
    QDateTime m_waitDeadline;
    bool m_waitingForDevice = false;
};
