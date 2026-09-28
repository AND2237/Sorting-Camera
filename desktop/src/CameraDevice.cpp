#include "CameraDevice.h"

#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "FrameBus.h"
#include "MjpegClient.h"
#include "Recorder.h"
#include "SessionState.h"
#include "SnapshotWriter.h"
#include "StreamStats.h"

#include <QDateTime>

CameraDevice::CameraDevice(const QString &deviceId, const QString &address, quint16 controlPort,
                           quint16 streamPort, DiscoveryService *discovery, QObject *parent)
    : QObject(parent)
    , m_deviceId(deviceId)
    , m_address(address)
    , m_controlPort(controlPort)
    , m_streamPort(streamPort)
    , m_frames(new FrameBus(this))
    , m_stream(new MjpegClient(this))
    , m_status(new DeviceStatus(this))
    , m_session(new SessionState(this))
    , m_recorder(new Recorder(this))
    , m_snapshots(new SnapshotWriter(m_frames, this))
    , m_stats(new StreamStats(m_stream, m_frames, this))
{
    m_session->observe(m_stream, m_status, discovery);

    // The bus keeps both views of one frame: the decoded image for the display
    // and the untouched socket bytes for the recorder. Snapshotting reads the
    // same bytes, which is what makes a snapshot the picture that was on
    // screen rather than a re-encode of it.
    connect(m_stream, &MjpegClient::frameReady, this,
            [this](const QImage &image, const QByteArray &raw, qint64 completeMs) {
                m_frames->setFrame(image, raw, completeMs);
                if (m_recorder->isRecording() && !raw.isEmpty()) {
                    m_recorder->appendFrame(raw, int(m_recorder->framesWritten()),
                                            QDateTime::currentMSecsSinceEpoch(), image.width(),
                                            image.height());
                }
            });

    // The camera asked to be re-initialised (a stalled capture task). Recovery
    // goes through the control channel, which is per device, so it uses this
    // device's own port rather than whatever was configured at startup.
    connect(m_stream, &MjpegClient::deviceRecoveryRequested, m_status,
            [this](const QString &host, quint16) {
                m_status->startPolling(host, m_controlPort);
                m_status->requestCameraRecovery();
            },
            Qt::QueuedConnection);
}

CameraDevice::~CameraDevice() = default;

QString CameraDevice::deviceId() const
{
    return m_deviceId;
}

QString CameraDevice::address() const
{
    return m_address;
}

quint16 CameraDevice::controlPort() const
{
    return m_controlPort;
}

quint16 CameraDevice::streamPort() const
{
    return m_streamPort;
}

void CameraDevice::setEndpoint(const QString &address, quint16 controlPort, quint16 streamPort)
{
    if (address == m_address && controlPort == m_controlPort && streamPort == m_streamPort) {
        return;
    }
    m_address = address;
    m_controlPort = controlPort;
    m_streamPort = streamPort;
    emit endpointChanged();
}

FrameBus *CameraDevice::frames() const
{
    return m_frames;
}

MjpegClient *CameraDevice::stream() const
{
    return m_stream;
}

DeviceStatus *CameraDevice::status() const
{
    return m_status;
}

SessionState *CameraDevice::session() const
{
    return m_session;
}

Recorder *CameraDevice::recorder() const
{
    return m_recorder;
}

SnapshotWriter *CameraDevice::snapshots() const
{
    return m_snapshots;
}

StreamStats *CameraDevice::stats() const
{
    return m_stats;
}
