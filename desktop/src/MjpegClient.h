#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>

class QThread;
class QTimer;

class MjpegWorker;

class MjpegClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(quint32 framesReceived READ framesReceived NOTIFY statsChanged)
    Q_PROPERTY(quint32 framesDropped READ framesDropped NOTIFY statsChanged)
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY statsChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(bool reconnecting READ isReconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(bool connecting READ isConnecting NOTIFY connectingChanged)
    Q_PROPERTY(int retryAttempt READ retryAttempt NOTIFY retryAttemptChanged)
    Q_PROPERTY(int noResponseStreak READ noResponseStreak NOTIFY noResponseStreakChanged)
    Q_PROPERTY(QString recoveryHint READ recoveryHint NOTIFY recoveryHintChanged)
    Q_PROPERTY(bool userConnected READ isUserConnected NOTIFY userConnectedChanged)
    Q_PROPERTY(int retryDelayMs READ retryDelayMs NOTIFY retryAttemptChanged)
    Q_PROPERTY(int maxRetries READ maxRetries CONSTANT)

public:
    explicit MjpegClient(QObject *parent = nullptr);
    ~MjpegClient() override;

    bool isActive() const;
    quint32 framesReceived() const;
    quint32 framesDropped() const;
    qint64 bytesReceived() const;
    QString errorString() const;
    bool isReconnecting() const;
    bool isConnecting() const;
    int retryAttempt() const;
    int noResponseStreak() const;
    QString recoveryHint() const;
    bool isUserConnected() const;
    int retryDelayMs() const;
    int maxRetries() const;

    Q_INVOKABLE void start(const QString &host, quint16 port);
    Q_INVOKABLE void stop();

signals:
    void activeChanged();
    void statsChanged();
    void errorStringChanged();
    void reconnectingChanged();
    void connectingChanged();
    void retryAttemptChanged();
    void noResponseStreakChanged();
    void recoveryHintChanged();
    void userConnectedChanged();
    void frameReady(const QImage &image, const QByteArray &raw, qint64 completeMs);
    void deviceRecoveryRequested(const QString &host, quint16 port);

private:
    void setError(const QString &err);
    void setReconnecting(bool reconnecting);
    void setConnecting(bool connecting);
    void setRetryAttempt(int attempt);
    void setNoResponseStreak(int streak);
    void setRecoveryHint(const QString &hint);
    void cancelRetry();
    void invokeStart();

    QThread *m_thread;
    MjpegWorker *m_worker;
    QTimer *m_retryTimer = nullptr;
    bool m_active = false;
    quint32 m_framesReceived = 0;
    quint32 m_framesDropped = 0;
    qint64 m_bytesReceived = 0;
    QString m_errorString;
    QString m_host;
    quint16 m_port = 0;
    bool m_userConnected = false;
    bool m_reconnecting = false;
    bool m_connecting = false;
    int m_retryAttempt = 0;
    int m_noResponseStreak = 0;
    int m_recoveriesTriggered = 0;
    QString m_recoveryHint;
};
