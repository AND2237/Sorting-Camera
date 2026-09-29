#include "CameraDevice.h"

#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "FrameBus.h"
#include "MjpegClient.h"
#include "NotificationCenter.h"
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
    , m_notifications(new NotificationCenter(this))
{
    m_session->observe(m_stream, m_status, discovery);

    // Every logged warning or worse is also worth seeing on screen, and the two
    // use one severity vocabulary so a condition cannot be a warning in the log
    // and an error on screen. Information is left out on purpose: a user does
    // not need a card for every poll that succeeded.
    connect(m_session, &SessionState::changed, this, [this]() {
        Diagnostics::Scope scope(Diagnostics::Category::Connection);
        switch (m_session->state()) {
        case SessionState::State::Error:
            m_notifications->postOnce(QStringLiteral("session-error"),
                                      Diagnostics::Level::Error, Diagnostics::Category::Connection,
                                      m_session->detail());
            break;
        case SessionState::State::Degraded:
            m_notifications->postOnce(QStringLiteral("session-degraded"),
                                      Diagnostics::Level::Warning, Diagnostics::Category::Connection,
                                      m_session->detail());
            break;
        case SessionState::State::Authenticated:
            m_notifications->postOnce(QStringLiteral("session-auth"),
                                      Diagnostics::Level::Info, Diagnostics::Category::Auth,
                                      QStringLiteral("Control channel authenticated"));
            break;
        default:
            break;
        }
        // A message that outlives its cause is worse than no message: the next
        // real fault arrives while an old one is still on screen, and the two
        // are read together. Every state that is not a fault retracts the fault
        // messages, so a card disappears on its own the moment the condition it
        // describes stops being true - which is also the moment nobody is
        // watching the screen for it.
        if (m_session->state() != SessionState::State::Error
            && m_session->state() != SessionState::State::Degraded) {
            m_notifications->dismissKey(QStringLiteral("session-error"));
            m_notifications->dismissKey(QStringLiteral("session-degraded"));
        }
    });

    connect(m_recorder, &Recorder::errorStringChanged, this, [this]() {
        const QString err = m_recorder->errorString();
        if (err.isEmpty()) {
            return;
        }
        m_notifications->post(Diagnostics::Level::Error, Diagnostics::Category::Recording, err);
    });

    connect(m_status, &DeviceStatus::configErrorChanged, this, [this]() {
        if (m_status->configError().isEmpty()) {
            // The next successful write clears the error. Retracting the card
            // here is what makes "fixed" mean gone rather than struck through.
            m_notifications->dismissKey(QStringLiteral("config"));
            return;
        }
        m_notifications->postOnce(QStringLiteral("config"),
                                  Diagnostics::Level::Error, Diagnostics::Category::Config,
                                  m_status->configError());
    });

    // Frames are the one quantity that would flood a log at 20 fps, so the
    // size and the pipeline age of each frame go to counters and never become a
    // line. The spread the counters keep is what makes a periodic report worth
    // reading: a p95 by eye instead of a single last value.
    connect(m_stream, &MjpegClient::frameReady, this,
            [this](const QImage &, const QByteArray &raw, qint64 completeMs) {
                if (Diagnostics::Facility *f = Diagnostics::Facility::instance()) {
                    f->countFrame(QStringLiteral("frame.bytes"), raw.size());
                    f->countFrame(QStringLiteral("frame.age_ms"),
                                  QDateTime::currentMSecsSinceEpoch() - completeMs);
                }
            });

    // Drops arrive as a running total, so the change since the last sample is
    // what gets counted - counting the total each time would report a number
    // that grows whether or not anything was dropped.
    connect(m_stream, &MjpegClient::statsChanged, this, [this]() {
        if (Diagnostics::Facility *f = Diagnostics::Facility::instance()) {
            const qint64 dropped = m_stream->framesDropped();
            const qint64 delta = dropped - m_lastDrops;
            if (delta > 0) {
                f->countDrop(QStringLiteral("stale"), delta);
            }
            m_lastDrops = dropped;
            f->countFrame(QStringLiteral("frames.received"), m_stream->framesReceived());
        }
    });

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
    // Delivered on the network thread, which is where architecture.md:124-128
    // puts the frame bus and the recorder. An Auto connection would queue this
    // to the GUI thread and put both of them there with it (CP-3). Nothing in
    // this lambda touches CameraDevice's own state: FrameBus::setFrame is
    // mutex-guarded and Recorder is internally synchronised.
    connect(m_stream, &MjpegClient::frameReady, this,
            [this](const QImage &image, const QByteArray &raw, qint64 completeMs) {
                m_frames->setFrame(image, raw, completeMs);
                if (m_recorder->isRecording() && !raw.isEmpty()) {
                    m_recorder->appendFrame(raw, int(m_recorder->framesWritten()),
                                            QDateTime::currentMSecsSinceEpoch(), image.width(),
                                            image.height());
                }
            },
            Qt::DirectConnection);

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

NotificationCenter *CameraDevice::notifications() const
{
    return m_notifications;
}
