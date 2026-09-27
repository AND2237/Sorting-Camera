#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QDateTime;
class QThread;

class DeviceStatusWorker;

class DeviceStatus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool online READ isOnline NOTIFY onlineChanged)
    Q_PROPERTY(QJsonObject status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QString configError READ configError NOTIFY configErrorChanged)
    Q_PROPERTY(bool configBusy READ configBusy NOTIFY configBusyChanged)

public:
    explicit DeviceStatus(QObject *parent = nullptr);
    ~DeviceStatus() override;

    bool isOnline() const;
    QJsonObject status() const;
    QString errorString() const;
    QString configError() const;
    bool configBusy() const;

    Q_INVOKABLE void startPolling(const QString &host, quint16 port);
    Q_INVOKABLE void stopPolling();
    Q_INVOKABLE void setResolution(const QString &framesizeKey);
    Q_INVOKABLE void setQuality(int quality);
    Q_INVOKABLE void requestCameraRecovery();
    Q_INVOKABLE void setFrameBufferCount(int fbCount);
    Q_INVOKABLE void setGrabMode(const QString &mode);
    Q_INVOKABLE void setXclk(int mhz);

signals:
    void onlineChanged();
    void statusChanged();
    void errorStringChanged();
    void configErrorChanged();
    void configBusyChanged();

private:
    void setConfigQuery(const QString &query);
    void trySendPendingConfig();

    QThread *m_thread;
    DeviceStatusWorker *m_worker;
    bool m_online = false;
    QJsonObject m_status;
    QString m_errorString;
    QString m_configError;
    bool m_configBusy = false;
    QString m_host;
    quint16 m_port = 0;
    QString m_pendingQuery;
    QDateTime m_waitDeadline;
    bool m_waitingForDevice = false;
};
