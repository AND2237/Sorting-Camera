#pragma once

#include "Diagnostics.h"

#include <QObject>
#include <QString>

class DeviceStatus;
class DiscoveryService;
class MjpegClient;

// Master Prompt 24. One explicit session state derived from three independent
// observers (discovery, control plane, video transport), so the UI reads a
// single token instead of inferring meaning from a pile of booleans.

struct SessionInput
{
    // video transport
    bool wantStream = false;
    bool active = false;
    bool connecting = false;
    bool reconnecting = false;
    QString streamError;
    int retryAttempt = 0;
    int retryDelayMs = 0;
    int maxRetries = 5;
    int noResponseStreak = 0;

    // control plane
    bool polling = false;
    bool online = false;
    bool authRequired = false;
    bool authenticated = false;
    bool authBusy = false;
    QString authError;
    QString configError;

    // discovery
    bool scanning = false;
    int deviceCount = 0;
};

struct SessionOutcome
{
    int state = 0;
    QString detail;
    QString severity = QStringLiteral("idle");

    bool operator==(const SessionOutcome &o) const
    {
        return state == o.state && detail == o.detail && severity == o.severity;
    }
    bool operator!=(const SessionOutcome &o) const { return !(*this == o); }
};

class SessionState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ stateName NOTIFY changed)
    Q_PROPERTY(QString label READ label NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(QString severity READ severity NOTIFY changed)
    Q_PROPERTY(int retryAttempt READ retryAttempt NOTIFY changed)
    Q_PROPERTY(int retryDelayMs READ retryDelayMs NOTIFY changed)
    Q_PROPERTY(int maxRetries READ maxRetries CONSTANT)

public:
    enum State {
        Discovering = 0,
        Connecting,
        Authenticated,
        Streaming,
        Degraded,
        Reconnecting,
        Disconnected,
        Error,
    };
    Q_ENUM(State)

    explicit SessionState(QObject *parent = nullptr);

    void observe(MjpegClient *stream, DeviceStatus *device, DiscoveryService *discovery);

    QString stateName() const;
    QString label() const;
    QString detail() const;
    QString severity() const;
    int retryAttempt() const;
    int retryDelayMs() const;
    int maxRetries() const;
    // How many times recompute() has run since observe(). Frame stats used to
    // be hooked, which drove this once per incoming frame while changing
    // nothing; a counter is what lets that stay out of the code (CP-13).
    qint64 recomputeCount() const { return m_recomputeCount; }

    State state() const { return static_cast<State>(m_outcome.state); }

    static SessionOutcome derive(const SessionInput &in);
    static const char *stateName(State s);
    static const char *stateLabel(State s);
    // How bad does a severity look on screen? The state machine owns the
    // "ok"/"warn"/"err" vocabulary and derive() already sets it per branch -
    // "the camera requires a sign-in" is deliberately a warning because it is
    // actionable, not a fault. A consumer that dispatched on state() alone
    // mapped every SessionState::Error to a red, sticky, hand-dismissed card
    // and threw that judgement away (DX-1). Only an explicit "warn" downgrades;
    // anything else stays an error, so a downgrade has to be asked for by name.
    static Diagnostics::Level notificationLevel(const QString &severity);

signals:
    void changed();

private:
    void recompute();

    MjpegClient *m_stream = nullptr;
    DeviceStatus *m_device = nullptr;
    DiscoveryService *m_discovery = nullptr;
    SessionOutcome m_outcome;
    qint64 m_recomputeCount = 0;
};
