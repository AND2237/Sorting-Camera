#pragma once

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

    State state() const { return static_cast<State>(m_outcome.state); }

    static SessionOutcome derive(const SessionInput &in);
    static const char *stateName(State s);
    static const char *stateLabel(State s);

signals:
    void changed();

private:
    void recompute();

    MjpegClient *m_stream = nullptr;
    DeviceStatus *m_device = nullptr;
    DiscoveryService *m_discovery = nullptr;
    SessionOutcome m_outcome;
};
