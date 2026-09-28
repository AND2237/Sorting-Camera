#include "SessionState.h"

#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "MjpegClient.h"

#include <QDebug>

namespace {
const char *kStateNames[] = {
    "discovering", "connecting", "authenticated", "streaming",
    "degraded",    "reconnecting", "disconnected", "error",
};
const char *kStateLabels[] = {
    "Discovering", "Connecting", "Authenticated", "Streaming",
    "Degraded",    "Reconnecting", "Disconnected", "Error",
};
} // namespace

const char *SessionState::stateName(State s)
{
    return kStateNames[s];
}

const char *SessionState::stateLabel(State s)
{
    return kStateLabels[s];
}

// The order of these branches is the contract: a later condition may never
// mask an earlier one. Exhausted retries outrank a reachable control plane,
// and a reachable plane outranks discovery, because those are exactly the
// cases a user needs told the truth about rather than shown "searching".
SessionOutcome SessionState::derive(const SessionInput &in)
{
    SessionOutcome out;

    const bool streaming = in.active && !in.connecting && !in.reconnecting;
    const bool streamFailed = in.wantStream && !in.active && !in.connecting
                              && !in.reconnecting && !in.streamError.isEmpty();
    const bool authFailed = !in.authError.isEmpty() && !in.authenticated && !in.authBusy;
    const bool needsSignIn = in.authRequired && !in.authenticated && !in.authBusy
                             && in.polling;

    if (streamFailed) {
        out.state = Error;
        out.severity = QStringLiteral("err");
        out.detail = in.streamError;
    } else if (authFailed) {
        out.state = Error;
        out.severity = QStringLiteral("err");
        out.detail = in.authError;
    } else if (in.reconnecting) {
        out.state = Reconnecting;
        out.severity = QStringLiteral("warn");
        out.detail = QStringLiteral("attempt %1 of %2, retrying in %3 ms")
                         .arg(in.retryAttempt).arg(in.maxRetries).arg(in.retryDelayMs);
    } else if (in.connecting) {
        out.state = Connecting;
        out.severity = QStringLiteral("ok");
        out.detail = QStringLiteral("opening the stream");
    } else if (streaming) {
        if (in.noResponseStreak > 0) {
            out.state = Degraded;
            out.severity = QStringLiteral("warn");
            out.detail = QStringLiteral("no frame for %1 polls in a row").arg(in.noResponseStreak);
        } else if (needsSignIn) {
            out.state = Degraded;
            out.severity = QStringLiteral("warn");
            out.detail = QStringLiteral("video only - sign in to change settings");
        } else if (!in.online) {
            out.state = Degraded;
            out.severity = QStringLiteral("warn");
            out.detail = QStringLiteral("video is running but the control API is not answering");
        } else if (!in.configError.isEmpty()) {
            out.state = Degraded;
            out.severity = QStringLiteral("warn");
            out.detail = QStringLiteral("last setting was rejected: %1").arg(in.configError);
        } else {
            out.state = Streaming;
            out.severity = QStringLiteral("ok");
            out.detail = QStringLiteral("live video and control both up");
        }
    } else if (needsSignIn) {
        // Actionable by the user, so a warning rather than a fault.
        out.state = Error;
        out.severity = QStringLiteral("warn");
        out.detail = QStringLiteral("the camera requires a sign-in before it will accept commands");
    } else if (in.online) {
        out.state = Authenticated;
        out.severity = QStringLiteral("ok");
        out.detail = QStringLiteral("control API reachable, video not started");
    } else if (in.scanning) {
        out.state = Discovering;
        out.detail = in.deviceCount > 0
                         ? QStringLiteral("%1 camera(s) announcing on the network").arg(in.deviceCount)
                         : QStringLiteral("waiting for a camera to announce");
    } else {
        out.state = Disconnected;
        out.detail = QStringLiteral("not connected and not searching");
    }
    return out;
}

SessionState::SessionState(QObject *parent) : QObject(parent)
{
    // Nothing has been observed yet, so the honest starting token is
    // "disconnected" rather than the enum's zero value (discovering).
    m_outcome.state = Disconnected;
}

void SessionState::observe(MjpegClient *stream, DeviceStatus *device,
                           DiscoveryService *discovery)
{
    m_stream = stream;
    m_device = device;
    m_discovery = discovery;

    // Everything observed here lives on the GUI thread, so these are direct
    // connections: state must be correct the moment QML reacts to a NOTIFY,
    // not one event-loop turn later, when it could briefly show a stale token.
    const auto hook = [this](auto *src, auto signal) {
        connect(src, signal, this, &SessionState::recompute);
    };
    if (m_stream) {
        hook(m_stream, &MjpegClient::activeChanged);
        hook(m_stream, &MjpegClient::statsChanged);
        hook(m_stream, &MjpegClient::errorStringChanged);
        hook(m_stream, &MjpegClient::reconnectingChanged);
        hook(m_stream, &MjpegClient::connectingChanged);
        hook(m_stream, &MjpegClient::retryAttemptChanged);
        hook(m_stream, &MjpegClient::noResponseStreakChanged);
        hook(m_stream, &MjpegClient::userConnectedChanged);
    }
    if (m_device) {
        hook(m_device, &DeviceStatus::onlineChanged);
        hook(m_device, &DeviceStatus::pollingChanged);
        hook(m_device, &DeviceStatus::errorStringChanged);
        hook(m_device, &DeviceStatus::configErrorChanged);
        hook(m_device, &DeviceStatus::configBusyChanged);
        hook(m_device, &DeviceStatus::authStateChanged);
    }
    if (m_discovery) {
        hook(m_discovery, &DiscoveryService::countChanged);
        hook(m_discovery, &DiscoveryService::scanningChanged);
    }
    recompute();
}

void SessionState::recompute()
{
    if (!m_stream || !m_device || !m_discovery) {
        return;
    }

    SessionInput in;
    in.wantStream = m_stream->isUserConnected();
    in.active = m_stream->isActive();
    in.connecting = m_stream->isConnecting();
    in.reconnecting = m_stream->isReconnecting();
    in.streamError = m_stream->errorString();
    in.retryAttempt = m_stream->retryAttempt();
    in.retryDelayMs = m_stream->retryDelayMs();
    in.maxRetries = m_stream->maxRetries();
    in.noResponseStreak = m_stream->noResponseStreak();

    in.polling = m_device->isPolling();
    in.online = m_device->isOnline();
    in.authRequired = m_device->isAuthRequired();
    in.authenticated = m_device->isAuthenticated();
    in.authBusy = m_device->isAuthBusy();
    in.authError = m_device->authError();
    in.configError = m_device->configError();

    in.scanning = m_discovery->isScanning();
    in.deviceCount = m_discovery->rowCount();

    const SessionOutcome out = derive(in);
    if (out == m_outcome) {
        return;
    }
    // One line per change only, never per poll: Master Prompt 38 forbids
    // flooding the log from a path that runs several times a second.
    if (m_outcome.state != out.state) {
        qDebug().noquote() << "[session]" << kStateNames[m_outcome.state] << "->"
                           << kStateNames[out.state] << ":" << out.detail;
    } else {
        qDebug().noquote() << "[session]" << kStateNames[out.state] << ":" << out.detail;
    }
    m_outcome = out;
    emit changed();
}

QString SessionState::stateName() const
{
    return QLatin1String(kStateNames[m_outcome.state]);
}

QString SessionState::label() const
{
    return QLatin1String(kStateLabels[m_outcome.state]);
}

QString SessionState::detail() const
{
    return m_outcome.detail;
}

QString SessionState::severity() const
{
    return m_outcome.severity;
}

int SessionState::retryAttempt() const
{
    return m_stream ? m_stream->retryAttempt() : 0;
}

int SessionState::retryDelayMs() const
{
    return m_stream ? m_stream->retryDelayMs() : 0;
}

int SessionState::maxRetries() const
{
    return m_stream ? m_stream->maxRetries() : 5;
}
