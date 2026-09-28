#pragma once

#include <QByteArray>
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

    // The exact bytes the camera sent, kept alongside the decoded image so
    // snapshot and recording never have to decode and re-encode (21, 22).
    QByteArray rawFrame() const;
    bool hasRawFrame() const;
    qint64 rawFrameCompleteMs() const;

public slots:
    void setFrame(const QImage &image, const QByteArray &raw, qint64 completeMs);

signals:
    void versionChanged();
    void fpsChanged();

private:
    mutable QMutex m_mutex;
    QImage m_image;
    QByteArray m_raw;
    int m_version = 0;
    double m_fps = 0.0;
    qint64 m_lastFrameMs = 0;
    qint64 m_lastFrameCompleteMs = 0;
    qint64 m_rawFrameCompleteMs = 0;
};
