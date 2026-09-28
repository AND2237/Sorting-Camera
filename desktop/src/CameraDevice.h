#pragma once

#include <QObject>
#include <QString>

class DeviceStatus;
class DiscoveryService;
class FrameBus;
class MjpegClient;
class Recorder;
class SessionState;
class SnapshotWriter;
class StreamStats;

// Master Prompt 27. One camera, and everything that belongs to it: its
// control channel, its stream, the latest frame, its metrics, its recording
// and its session state.
//
// The point of the class is that none of that hangs off a global. Two
// CameraDevice instances can exist at the same time with entirely separate
// transports, and the only thing that decides which one the window shows is
// DeviceRegistry::setActive(). The first version has a single view, which 27
// allows, but the objects behind it are not single-camera by construction.
class CameraDevice : public QObject
{
    Q_OBJECT

public:
    // deviceId is the camera's own identity and is what decides whether a
    // second acquire() is the same camera. An address is not: a camera that
    // comes back on a different address is still the same camera, and treating
    // it as a new one would strand its stored credential and its history.
    CameraDevice(const QString &deviceId, const QString &address, quint16 controlPort,
                 quint16 streamPort, DiscoveryService *discovery, QObject *parent = nullptr);
    ~CameraDevice() override;

    QString deviceId() const;
    QString address() const;
    quint16 controlPort() const;
    quint16 streamPort() const;

    // A discovery announce can carry a new address or a firmware that moved a
    // port; identity is unchanged.
    void setEndpoint(const QString &address, quint16 controlPort, quint16 streamPort);

    FrameBus *frames() const;
    MjpegClient *stream() const;
    DeviceStatus *status() const;
    SessionState *session() const;
    Recorder *recorder() const;
    SnapshotWriter *snapshots() const;
    StreamStats *stats() const;

signals:
    void endpointChanged();

private:
    QString m_deviceId;
    QString m_address;
    quint16 m_controlPort = 0;
    quint16 m_streamPort = 0;

    FrameBus *m_frames = nullptr;
    MjpegClient *m_stream = nullptr;
    DeviceStatus *m_status = nullptr;
    SessionState *m_session = nullptr;
    Recorder *m_recorder = nullptr;
    SnapshotWriter *m_snapshots = nullptr;
    StreamStats *m_stats = nullptr;
};
