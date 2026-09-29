#include "SessionState.h"

#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "MjpegClient.h"

#include <QSignalSpy>
#include <QTest>

using State = SessionState::State;

namespace {

SessionInput idle()
{
    SessionInput in;
    return in;
}

SessionInput online()
{
    SessionInput in;
    in.polling = true;
    in.online = true;
    in.authenticated = true;
    return in;
}

SessionInput live()
{
    SessionInput in = online();
    in.wantStream = true;
    in.active = true;
    return in;
}

} // namespace

class TestSessionState : public QObject
{
    Q_OBJECT

private slots:
    void stateNamesMatchMasterPrompt();
    void idleIsDisconnected();
    void scanningIsDiscovering();
    void connectingBeatsAuthenticated();
    void authenticatedIsControlOnly();
    void streamingWhenBothPlanesUp();
    void degradedOnFrameStall();
    void degradedWhenControlLost();
    void degradedOnVideoOnly();
    void degradedOnRejectedSetting();
    void reconnectingExposesBackoff();
    void signInRequiredIsAWarningNotAFault();
    void notificationLevelCarriesTheSeverityTheStateSet();
    void rejectedCredentialsAreAnError();
    void exhaustedRetriesAreAnError();
    void exhaustedRetriesOutrankAuthenticated();
    void reconnectingOutrankAuthenticated();
    void authenticatedOutranksDiscovering();
    void signRequiredOutranksDiscovering();
    void frameStatsNeverChangeTheSessionOutcome();
};

void TestSessionState::stateNamesMatchMasterPrompt()
{
    // The eight tokens are named in Master Prompt 24; they are the UI contract.
    const char *expected[] = {"discovering", "connecting", "authenticated", "streaming",
                              "degraded",    "reconnecting", "disconnected", "error"};
    for (int i = 0; i < 8; i++) {
        QCOMPARE(QString::fromLatin1(SessionState::stateName(static_cast<State>(i))),
                 QString::fromLatin1(expected[i]));
    }
    QCOMPARE(QString::fromLatin1(SessionState::stateLabel(State::Discovering)),
             QStringLiteral("Discovering"));
    QCOMPARE(QString::fromLatin1(SessionState::stateLabel(State::Degraded)),
             QStringLiteral("Degraded"));
    QCOMPARE(QString::fromLatin1(SessionState::stateLabel(State::Error)),
             QStringLiteral("Error"));
}

void TestSessionState::idleIsDisconnected()
{
    const auto out = SessionState::derive(idle());
    QCOMPARE(out.state, int(State::Disconnected));
    QCOMPARE(out.severity, QStringLiteral("idle"));
}

void TestSessionState::scanningIsDiscovering()
{
    SessionInput in;
    in.scanning = true;
    auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Discovering));
    QVERIFY(out.detail.contains(QStringLiteral("waiting")));

    in.deviceCount = 3;
    out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Discovering));
    QVERIFY(out.detail.contains(QStringLiteral("3 camera")));
}

void TestSessionState::connectingBeatsAuthenticated()
{
    SessionInput in = online();
    in.wantStream = true;
    in.connecting = true;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Connecting));
    QCOMPARE(out.severity, QStringLiteral("ok"));
}

void TestSessionState::authenticatedIsControlOnly()
{
    const auto out = SessionState::derive(online());
    QCOMPARE(out.state, int(State::Authenticated));
    QCOMPARE(out.severity, QStringLiteral("ok"));
    QVERIFY(out.detail.contains(QStringLiteral("control API reachable")));
}

void TestSessionState::streamingWhenBothPlanesUp()
{
    const auto out = SessionState::derive(live());
    QCOMPARE(out.state, int(State::Streaming));
    QCOMPARE(out.severity, QStringLiteral("ok"));
}

void TestSessionState::degradedOnFrameStall()
{
    SessionInput in = live();
    in.noResponseStreak = 3;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Degraded));
    QCOMPARE(out.severity, QStringLiteral("warn"));
    QVERIFY(out.detail.contains(QStringLiteral("3 polls")));
}

void TestSessionState::degradedWhenControlLost()
{
    SessionInput in = live();
    in.online = false;
    in.authenticated = false;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Degraded));
    QVERIFY(out.detail.contains(QStringLiteral("control API is not answering")));
}

void TestSessionState::degradedOnVideoOnly()
{
    // The stream port is deliberately unauthenticated, so a camera can be
    // watchable while its control plane refuses us. That is degraded, not
    // streaming and not an error - the user must be told why settings are dead.
    SessionInput in = live();
    in.online = false;
    in.authenticated = false;
    in.authRequired = true;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Degraded));
    QVERIFY(out.detail.contains(QStringLiteral("video only")));
}

void TestSessionState::degradedOnRejectedSetting()
{
    SessionInput in = live();
    in.configError = QStringLiteral("quality out of range");
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Degraded));
    QVERIFY(out.detail.contains(QStringLiteral("quality out of range")));
}

void TestSessionState::reconnectingExposesBackoff()
{
    SessionInput in = live();
    in.active = false;
    in.reconnecting = true;
    in.retryAttempt = 3;
    in.retryDelayMs = 2000;
    in.maxRetries = 5;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Reconnecting));
    QCOMPARE(out.severity, QStringLiteral("warn"));
    QVERIFY(out.detail.contains(QStringLiteral("attempt 3 of 5")));
    QVERIFY(out.detail.contains(QStringLiteral("2000 ms")));
}

void TestSessionState::signInRequiredIsAWarningNotAFault()
{
    SessionInput in;
    in.polling = true;
    in.authRequired = true;
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Error));
    QCOMPARE(out.severity, QStringLiteral("warn"));
    QVERIFY(out.detail.contains(QStringLiteral("requires a sign-in")));
}

void TestSessionState::notificationLevelCarriesTheSeverityTheStateSet()
{
    // Two outcomes are both SessionState::Error. One is a fault, one is an
    // ordinary first-run condition the operator can act on, and derive() has
    // already said which is which. A consumer that dispatched on state() alone
    // threw that judgement away and made "the camera requires a sign-in" a
    // permanent red card that had to be cleared by hand on every connect
    // (DX-1).
    SessionInput signIn;
    signIn.polling = true;
    signIn.authRequired = true;
    const auto needSignIn = SessionState::derive(signIn);
    QCOMPARE(needSignIn.state, int(State::Error));
    QCOMPARE(SessionState::notificationLevel(needSignIn.severity), Diagnostics::Level::Warning);

    SessionInput refused;
    refused.polling = true;
    refused.authRequired = true;
    refused.authError = QStringLiteral("wrong password");
    const auto fault = SessionState::derive(refused);
    QCOMPARE(fault.state, int(State::Error));
    QCOMPARE(SessionState::notificationLevel(fault.severity), Diagnostics::Level::Error);

    // Degraded carries severity "warn" in every branch of derive(); the mapping
    // must keep it a warning rather than promoting it to a fault.
    SessionInput stalling = live();
    stalling.noResponseStreak = 3;
    const auto degraded = SessionState::derive(stalling);
    QCOMPARE(degraded.state, int(State::Degraded));
    QCOMPARE(SessionState::notificationLevel(degraded.severity), Diagnostics::Level::Warning);

    // Failing closed: only an explicit "warn" demotes, so an unrecognised or
    // absent severity never silently downgrades a real fault.
    QCOMPARE(SessionState::notificationLevel(QStringLiteral("err")), Diagnostics::Level::Error);
    QCOMPARE(SessionState::notificationLevel(QStringLiteral("ok")), Diagnostics::Level::Error);
    QCOMPARE(SessionState::notificationLevel(QStringLiteral("idle")), Diagnostics::Level::Error);
    QCOMPARE(SessionState::notificationLevel(QString()), Diagnostics::Level::Error);
}

void TestSessionState::rejectedCredentialsAreAnError()
{
    SessionInput in;
    in.polling = true;
    in.authRequired = true;
    in.authError = QStringLiteral("wrong password");
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Error));
    QCOMPARE(out.severity, QStringLiteral("err"));
    QCOMPARE(out.detail, QStringLiteral("wrong password"));
}

void TestSessionState::exhaustedRetriesAreAnError()
{
    SessionInput in;
    in.wantStream = true;
    in.streamError = QStringLiteral("connection refused");
    const auto out = SessionState::derive(in);
    QCOMPARE(out.state, int(State::Error));
    QCOMPARE(out.severity, QStringLiteral("err"));
    QCOMPARE(out.detail, QStringLiteral("connection refused"));
}

void TestSessionState::exhaustedRetriesOutrankAuthenticated()
{
    // A stale error string must never let the UI say the camera is fine.
    SessionInput in = online();
    in.wantStream = true;
    in.streamError = QStringLiteral("connection refused");
    QCOMPARE(SessionState::derive(in).state, int(State::Error));
}

void TestSessionState::reconnectingOutrankAuthenticated()
{
    SessionInput in = online();
    in.wantStream = true;
    in.active = false;
    in.reconnecting = true;
    in.retryAttempt = 1;
    QCOMPARE(SessionState::derive(in).state, int(State::Reconnecting));
}

void TestSessionState::authenticatedOutranksDiscovering()
{
    // Discovery runs continuously; once the control plane answers, the token
    // must stop claiming we are still looking.
    SessionInput in = online();
    in.scanning = true;
    in.deviceCount = 2;
    QCOMPARE(SessionState::derive(in).state, int(State::Authenticated));
}

void TestSessionState::signRequiredOutranksDiscovering()
{
    SessionInput in;
    in.polling = true;
    in.authRequired = true;
    in.scanning = true;
    in.deviceCount = 1;
    QCOMPARE(SessionState::derive(in).state, int(State::Error));
}

void TestSessionState::frameStatsNeverChangeTheSessionOutcome()
{
    // CP-13. statsChanged fires on every readyRead(), i.e. once per incoming
    // frame, and SessionState used to recompute on it: fifteen cross-object
    // property reads plus a full derive() building QString::arg() detail
    // strings, all of it thrown away by the "outcome unchanged" early-out,
    // roughly 600 times a second while the stream ran. The hook is gone, and
    // this pins the invariant that made removing it safe: no frame counter
    // reaches SessionInput, so stats can never change the outcome. If a field
    // is ever added that does, this test fails before the UI can freeze.
    DiscoveryService discovery;
    DeviceStatus device;
    MjpegClient stream;

    SessionState state;
    state.observe(&stream, &device, &discovery);

    QSignalSpy changed(&state, &SessionState::changed);
    const qint64 before = state.recomputeCount();
    for (int i = 0; i < 50; ++i) {
        stream.statsChanged();
    }
    // Neither the outcome nor the recompute count may move: the cost CP-13 was
    // about was the recompute itself, not the signal that was already discarded
    // by the "outcome unchanged" early-out.
    QCOMPARE(changed.count(), 0);
    QCOMPARE(state.recomputeCount(), before);

    // The wiring itself is still live: a signal that does feed SessionInput
    // recomputes and moves the token.
    const State beforeState = state.state();
    discovery.startScanning();
    QVERIFY(state.recomputeCount() > before);
    QVERIFY(changed.count() > 0);
    QVERIFY(state.state() != beforeState);
    discovery.stopScanning();
}

QTEST_GUILESS_MAIN(TestSessionState)
#include "tst_sessionstate.moc"
