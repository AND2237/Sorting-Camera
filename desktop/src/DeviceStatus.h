#pragma once

#include <QJsonObject>
#include <QMap>
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
    Q_PROPERTY(QJsonObject capabilities READ capabilities NOTIFY capabilitiesChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QString configError READ configError NOTIFY configErrorChanged)
    Q_PROPERTY(bool configBusy READ configBusy NOTIFY configBusyChanged)
    Q_PROPERTY(bool authRequired READ isAuthRequired NOTIFY authStateChanged)
    Q_PROPERTY(bool authenticated READ isAuthenticated NOTIFY authStateChanged)
    Q_PROPERTY(bool authBusy READ isAuthBusy NOTIFY authStateChanged)
    Q_PROPERTY(QString authError READ authError NOTIFY authStateChanged)
    Q_PROPERTY(bool credentialStored READ isCredentialStored NOTIFY authStateChanged)
    Q_PROPERTY(bool credentialStorageAvailable READ credentialStorageAvailable NOTIFY authStateChanged)
    // True while a sensor control read-modify-write is in flight; the controls
    // panel disables itself on it so two drags cannot race each other.
    Q_PROPERTY(bool sensorBusy READ isSensorBusy NOTIFY sensorStateChanged)
    Q_PROPERTY(QString sensorError READ sensorError NOTIFY sensorStateChanged)

public:
    explicit DeviceStatus(QObject *parent = nullptr);
    ~DeviceStatus() override;

    bool isOnline() const;
    bool isPolling() const;
    QJsonObject status() const;
    QJsonObject capabilities() const;
    QString errorString() const;
    QString configError() const;
    bool configBusy() const;
    bool isAuthRequired() const;
    bool isAuthenticated() const;
    bool isAuthBusy() const;
    QString authError() const;
    bool isCredentialStored() const;
    bool credentialStorageAvailable() const;
    bool isSensorBusy() const;
    QString sensorError() const;

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
    // One sensor control write, and a restore of every supported control to the
    // value the firmware reports as its default. Both go through the sensor
    // endpoint rather than /config, because /config restarts the camera and a
    // brightness slider must not tear the stream down.
    Q_INVOKABLE void setSensorControl(const QString &name, int value);
    Q_INVOKABLE void resetSensorControls();

signals:
    void onlineChanged();
    void pollingChanged();
    void statusChanged();
    void capabilitiesChanged();
    void sensorStateChanged();
    void errorStringChanged();
    void configErrorChanged();
    void configBusyChanged();
    void authStateChanged();

private:
    void setConfigQuery(const QString &query);
    void trySendPendingConfig();
    void requestCapabilities();
    void refreshAuthState();
    QString hostKey() const;
    QString credentialKey() const;

    QThread *m_thread;
    DeviceStatusWorker *m_worker;
    bool m_online = false;
    bool m_polling = false;
    QJsonObject m_status;
    QJsonObject m_capabilities;
    QDateTime m_lastCapAttempt;
    QString m_sensorError;
    bool m_sensorBusy = false;
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
    // One pending config write is a batch, not a slot. Keyed so a second write
    // to the same key replaces the first and the batch still leaves as a
    // single request, which is what /api/v1/config is built for.
    QMap<QString, QString> m_pendingKv;
    bool m_configInFlight = false;
    bool m_statusFresh = false;
    int m_cfgDebounceGen = 0;
    QDateTime m_waitDeadline;
    QDateTime m_statusWaitDeadline;
};
