#include "StreamStats.h"

#include "AppMetrics.h"
#include "FrameBus.h"
#include "MjpegClient.h"

#include <QTimer>

StreamStats::StreamStats(MjpegClient *stream, FrameBus *bus, QObject *parent)
    : QObject(parent), m_stream(stream), m_bus(bus)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &StreamStats::sample);
    m_timer->start();

    // Bitrate must reset when the transport does, otherwise the first sample
    // after a reconnect reports the entire gap as if it were one second.
    if (m_stream) {
        connect(m_stream, &MjpegClient::activeChanged, this, [this]() {
            m_prevBytes = m_stream ? m_stream->bytesReceived() : 0;
            m_prevSampleMs = AppMetrics::nowMs();
            m_bitrateBps = 0.0;
            emit statsChanged();
        });
    }
}

bool StreamStats::hasFrame() const
{
    return m_bus && m_bus->lastFrameCompleteMs() > 0;
}

double StreamStats::bitrateBps() const
{
    return m_bitrateBps;
}

double StreamStats::bitrateMbps() const
{
    return m_bitrateBps * 8.0 / 1000000.0;
}

int StreamStats::frameAgeMs() const
{
    if (!hasFrame()) {
        return 0;
    }
    return int(AppMetrics::nowMs() - m_bus->lastFrameCompleteMs());
}

void StreamStats::sample()
{
    if (!m_stream) {
        return;
    }
    const qint64 now = AppMetrics::nowMs();
    const qint64 bytes = m_stream->bytesReceived();
    if (m_prevSampleMs > 0 && now > m_prevSampleMs) {
        const double seconds = double(now - m_prevSampleMs) / 1000.0;
        const qint64 delta = bytes - m_prevBytes;
        if (delta > 0) {
            const double inst = double(delta) / seconds;
            // Smoothed so the readout is legible rather than jumping every
            // second. A window with no bytes at all reports exactly zero: an
            // idle or stalled stream is 0 Mbps, not a decaying fraction of the
            // last good reading.
            m_bitrateBps = m_bitrateBps <= 0.0 ? inst : m_bitrateBps * 0.6 + inst * 0.4;
        } else {
            m_bitrateBps = 0.0;
        }
    }
    m_prevBytes = bytes;
    m_prevSampleMs = now;
    emit statsChanged();
}
