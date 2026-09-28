#include "SessionState.h"

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
    void rejectedCredentialsAreAnError();
    void exhaustedRetriesAreAnError();
    void exhaustedRetriesOutrankAuthenticated();
    void reconnectingOutrankAuthenticated();
    void authenticatedOutranksDiscovering();
    void signRequiredOutranksDiscovering();
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

QTEST_GUILESS_MAIN(TestSessionState)
#include "tst_sessionstate.moc"
