#include "FrameBus.h"

#include <QDateTime>

FrameBus::FrameBus(QObject *parent)
    : QObject(parent)
{
}

int FrameBus::version() const
{
    QMutexLocker lock(&m_mutex);
    return m_version;
}

double FrameBus::fps() const
{
    QMutexLocker lock(&m_mutex);
    return m_fps;
}

QImage FrameBus::image() const
{
    QMutexLocker lock(&m_mutex);
    return m_image;
}

void FrameBus::setFrame(const QImage &image)
{
    if (image.isNull()) {
        return;
    }

    double fps = 0.0;
    {
        QMutexLocker lock(&m_mutex);
        m_image = image;
        ++m_version;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastFrameMs > 0) {
            const qint64 dt = now - m_lastFrameMs;
            if (dt > 0) {
                const double inst = 1000.0 / double(dt);
                m_fps = (m_fps <= 0.0) ? inst : m_fps * 0.8 + inst * 0.2;
            }
        }
        m_lastFrameMs = now;
        fps = m_fps;
    }

    emit versionChanged();
    emit fpsChanged();
    Q_UNUSED(fps);
}
