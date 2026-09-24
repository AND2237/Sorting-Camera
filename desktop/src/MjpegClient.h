#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class QThread;

class MjpegWorker;

class MjpegClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(quint32 framesReceived READ framesReceived NOTIFY statsChanged)
    Q_PROPERTY(quint32 framesDropped READ framesDropped NOTIFY statsChanged)
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY statsChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    explicit MjpegClient(QObject *parent = nullptr);
    ~MjpegClient() override;

    bool isActive() const;
    quint32 framesReceived() const;
    quint32 framesDropped() const;
    qint64 bytesReceived() const;
    QString errorString() const;

    Q_INVOKABLE void start(const QString &host, quint16 port);
    Q_INVOKABLE void stop();

signals:
    void activeChanged();
    void statsChanged();
    void errorStringChanged();
    void frameReady(const QImage &image);

private:
    QThread *m_thread;
    MjpegWorker *m_worker;
    bool m_active = false;
    quint32 m_framesReceived = 0;
    quint32 m_framesDropped = 0;
    qint64 m_bytesReceived = 0;
    QString m_errorString;
};
