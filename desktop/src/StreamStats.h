#pragma once

#include <QObject>

class FrameBus;
class MjpegClient;
class QTimer;

// Master Prompt 26 asks for bitrate and latency on screen. AppMetrics holds
// accumulators for the bench harness, not live rates, so this derives the two
// numbers a viewer needs from what the bus and the client already track.
//
// Latency here is frame age: how old the picture on screen is. It is the one
// latency number a user can actually act on, and it needs no extra protocol
// field to compute.
class StreamStats : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY statsChanged)
    Q_PROPERTY(double bitrateBps READ bitrateBps NOTIFY statsChanged)
    Q_PROPERTY(double bitrateMbps READ bitrateMbps NOTIFY statsChanged)
    Q_PROPERTY(int frameAgeMs READ frameAgeMs NOTIFY statsChanged)

public:
    StreamStats(MjpegClient *stream, FrameBus *bus, QObject *parent = nullptr);

    bool hasFrame() const;
    double bitrateBps() const;
    double bitrateMbps() const;
    int frameAgeMs() const;

    // Exposed so tests can sample without waiting on the wall clock.
    Q_INVOKABLE void sample();

signals:
    void statsChanged();

private:
    MjpegClient *m_stream = nullptr;
    FrameBus *m_bus = nullptr;
    QTimer *m_timer = nullptr;
    qint64 m_prevBytes = 0;
    qint64 m_prevSampleMs = 0;
    double m_bitrateBps = 0.0;
};
