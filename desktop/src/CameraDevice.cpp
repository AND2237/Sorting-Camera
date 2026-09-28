#include "CameraDevice.h"

#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "FrameBus.h"
#include "MjpegClient.h"
#include "ProfileEngine.h"
#include "Recorder.h"
#include "SessionState.h"
#include "SnapshotWriter.h"
#include "StreamStats.h"

#include <QDateTime>
#include <QJsonObject>
#include <QTimer>

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
    , m_profiles(new ProfileEngine(this))
{
    m_session->observe(m_stream, m_status, discovery);

    // The profile engine learns what the camera is actually doing from the
    // status payload rather than from what was requested: a profile is a
    // statement about an operating point, and only the camera knows which
    // point it ended up at after clamping or rejecting a request.
    connect(m_status, &DeviceStatus::statusChanged, this, [this]() {
        const QJsonObject st = m_status->status();
        const QString key = ProfileEngine::framesizeKeyFromResolution(
            st.value(QStringLiteral("resolution")).toString());
        if (key.isEmpty()) {
            return;
        }
        m_profiles->setConfig(key, st.value(QStringLiteral("quality")).toInt(),
                              st.value(QStringLiteral("xclk_mhz")).toInt(),
                              st.value(QStringLiteral("fb_count")).toInt(),
                              ProfileEngine::grabModeFromFirmware(
                                  st.value(QStringLiteral("grab_mode")).toString()));
    });
    connect(m_profiles, &ProfileEngine::activeChanged, this, [this]() {
        // Which ladder entry the camera turned out to be running, and the
        // measurement behind that entry. The evidence string goes to the log
        // rather than the screen because it cites benchmark runs, and a user
        // watching 10 fps deserves to know it was predicted.
        qInfo("[profile] active=%s measured=%.2f fps floor=%.0f fps %s (%s)",
              qPrintable(m_profiles->activeName()), m_profiles->activeMeasuredFps(),
              m_profiles->activeFloorFps(),
              m_profiles->activeMeetsFloor() ? "meets floor" : "UNDER FLOOR",
              qPrintable(m_profiles->activeEvidence()));
    });

    // One sample per second into the engine's shortfall counter. Sampling on
    // the frame path instead would feed it thousands of decisions a second and
    // make "three consecutive windows" meaningless.
    auto *ticker = new QTimer(this);
    ticker->setInterval(1000);
    connect(ticker, &QTimer::timeout, this, [this]() {
        if (m_stream->isActive() && m_frames->fps() > 0.0) {
            m_profiles->observeFps(m_frames->fps());
        } else {
            m_profiles->clearObservation();
        }
    });
    ticker->start();

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

ProfileEngine *CameraDevice::profiles() const
{
    return m_profiles;
}
