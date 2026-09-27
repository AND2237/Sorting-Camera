#pragma once

#include <QImage>
#include <QMutex>
#include <QObject>

class FrameBus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int version READ version NOTIFY versionChanged)
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)

public:
    explicit FrameBus(QObject *parent = nullptr);

    int version() const;
    double fps() const;
    QImage image() const;
    qint64 lastFrameCompleteMs() const;

public slots:
    void setFrame(const QImage &image, qint64 completeMs);

signals:
    void versionChanged();
    void fpsChanged();

private:
    mutable QMutex m_mutex;
    QImage m_image;
    int m_version = 0;
    double m_fps = 0.0;
    qint64 m_lastFrameMs = 0;
    qint64 m_lastFrameCompleteMs = 0;
};
