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
    // How many frames the consumer never saw. Latest-frame-wins is the correct
    // policy, but a drop that is not counted is a drop that cannot be
    // investigated, so it is reported here rather than being absorbed silently.
    Q_PROPERTY(int overwrittenCount READ overwrittenCount NOTIFY overwrittenChanged)

public:
    explicit FrameBus(QObject *parent = nullptr);

    int version() const;
    double fps() const;
    QImage image() const;
    qint64 lastFrameCompleteMs() const;
    int overwrittenCount() const;

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
    void overwrittenChanged();

private:
    mutable QMutex m_mutex;
    QImage m_image;
    QByteArray m_raw;
    int m_version = 0;
    double m_fps = 0.0;
    qint64 m_lastFrameMs = 0;
    qint64 m_lastFrameCompleteMs = 0;
    qint64 m_rawFrameCompleteMs = 0;
    // True while the stored frame has already been handed to a consumer. A new
    // frame arriving in that window replaces one nobody looked at, which is the
    // only sense in which a frame is "dropped" under latest-frame-wins.
    // Mutable because reading the frame is what marks it as seen, and the read
    // path is const by design - the bus is read from the render thread.
    mutable bool m_consumed = true;
    int m_overwritten = 0;
};
