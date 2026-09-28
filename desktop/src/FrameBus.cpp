#include "FrameBus.h"

#include "AppMetrics.h"

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
    // Reading the frame is what marks it as seen. Without this there is no way
    // to tell a frame nobody wanted from one that arrived too fast to display,
    // and the overwrites counter would be meaningless.
    m_consumed = true;
    return m_image;
}

int FrameBus::overwrittenCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_overwritten;
}

qint64 FrameBus::lastFrameCompleteMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastFrameCompleteMs;
}

QByteArray FrameBus::rawFrame() const
{
    QMutexLocker lock(&m_mutex);
    return m_raw;
}

bool FrameBus::hasRawFrame() const
{
    QMutexLocker lock(&m_mutex);
    return !m_raw.isEmpty();
}

qint64 FrameBus::rawFrameCompleteMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_rawFrameCompleteMs;
}

void FrameBus::setFrame(const QImage &image, const QByteArray &raw, qint64 completeMs)
{
    if (image.isNull()) {
        return;
    }

    double fps = 0.0;
    bool overwritten = false;
    {
        QMutexLocker lock(&m_mutex);
        // A frame still waiting to be displayed when the next one arrives was
        // never seen by anyone. Latest-frame-wins is the right policy for a live
        // view, but the replacement still has to be counted.
        if (!m_consumed && m_version > 0) {
            ++m_overwritten;
            overwritten = true;
            AppMetrics::instance().countOverwritten();
        }
        m_consumed = false;

        m_image = image;
        m_raw = raw;
        m_rawFrameCompleteMs = raw.isEmpty() ? 0 : completeMs;
        m_lastFrameCompleteMs = completeMs;
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

    AppMetrics::instance().countPresented();
    emit versionChanged();
    emit fpsChanged();
    if (overwritten) {
        emit overwrittenChanged();
    }
    Q_UNUSED(fps);
}
