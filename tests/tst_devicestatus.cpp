// Section 26 asks for camera configuration controls, and those controls only
// exist if the desktop takes their shape from what the camera advertises
// rather than from a hard-coded list. This test drives DeviceStatus against a
// stub camera and pins that contract: capabilities arrive, a write reaches the
// right endpoint with the right body, an unknown control never leaves the PC,
// and a device switch cannot leave stale controls on screen.
#include "DeviceStatus.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <utility>

namespace {

// Stands in for the camera's control port. Records every request so a test can
// assert on the path and body, and answers with a fixture that matches the
// shape camera_control_add_capabilities() produces on the device.
class ControlApiStub : public QObject
{
    Q_OBJECT
public:
    explicit ControlApiStub(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &ControlApiStub::onConnection);
        connect(&m_server, &QTcpServer::pendingConnectionAvailable, this,
                &ControlApiStub::onConnection);
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return m_server.serverPort(); }

    struct Request
    {
        QString method;
        QString path;
        QString contentType;
        QByteArray body;
    };

    QList<Request> requests;
    bool authRequired = false;
    int capabilitiesStatus = 200;
    int sensorPostStatus = 200;
    QString sensorPostError = QStringLiteral("rejected");
    // Turns /api/v1/config into a 401 until a sign-in succeeds, which is what
    // the camera looks like after a restart that dropped its token table.
    bool configUnauthorized = false;
    int configRequests = 0;
    int loginRequests = 0;

    const QJsonObject capabilities = QJsonObject{
        {QStringLiteral("controls"),
         QJsonObject{
             {QStringLiteral("brightness"),
              QJsonObject{{QStringLiteral("supported"), true},
                          {QStringLiteral("group"), QStringLiteral("image")},
                          {QStringLiteral("min"), -2},
                          {QStringLiteral("max"), 2},
                          {QStringLiteral("default"), 0}}},
             {QStringLiteral("contrast"),
              QJsonObject{{QStringLiteral("supported"), true},
                          {QStringLiteral("group"), QStringLiteral("image")},
                          {QStringLiteral("min"), -2},
                          {QStringLiteral("max"), 2},
                          {QStringLiteral("default"), 0}}},
             {QStringLiteral("hmirror"),
              QJsonObject{{QStringLiteral("supported"), true},
                          {QStringLiteral("group"), QStringLiteral("flip")},
                          {QStringLiteral("min"), 0},
                          {QStringLiteral("max"), 1},
                          {QStringLiteral("default"), 0}}},
             // Present in the payload but unusable: the panel must not offer it.
             {QStringLiteral("colorbar"),
              QJsonObject{{QStringLiteral("supported"), false},
                          {QStringLiteral("group"), QStringLiteral("test")},
                          {QStringLiteral("min"), 0},
                          {QStringLiteral("max"), 1},
                          {QStringLiteral("default"), 0}}},
         }},
        {QStringLiteral("features"), QJsonObject{}}};

    const QJsonObject status = QJsonObject{
        {QStringLiteral("device_id"), QStringLiteral("b4bfe9343ae0")},
        {QStringLiteral("fw_version"), QStringLiteral("v0.1.0-7-ga376882")},
        {QStringLiteral("resolution"), QStringLiteral("1280x720")},
        {QStringLiteral("quality"), 12},
        {QStringLiteral("brightness"), 0},
        {QStringLiteral("contrast"), 0},
        {QStringLiteral("hmirror"), 0},
        {QStringLiteral("stream_clients"), 0},
    };

private:
    void onConnection()
    {
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock) {
            return;
        }
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            m_buffer[sock] += sock->readAll();
            handle(sock);
        });
    }

    void handle(QTcpSocket *sock)
    {
        QByteArray &buf = m_buffer[sock];
        // Header block first, then any body the headers promised.
        int end = buf.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        const QByteArray head = buf.left(end + 4);
        const int contentLength = parseContentLength(head);
        if (buf.size() < end + 4 + contentLength) {
            return;
        }

        const QByteArray body = buf.mid(end + 4, contentLength);
        buf.remove(0, end + 4 + contentLength);

        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        Request req;
        req.method = QString::fromUtf8(first.value(0));
        req.path = QString::fromUtf8(first.value(1));
        req.contentType = QString::fromUtf8(headerValue(head, "content-type"));
        req.body = body;
        requests.append(req);

        if (authRequired && !req.path.startsWith(QStringLiteral("/api/v1/auth/"))) {
            respond(sock, 401, QByteArray("{\"error\":\"unauthorized\"}"));
            return;
        }
        if (req.path == QStringLiteral("/api/v1/capabilities")) {
            if (capabilitiesStatus != 200) {
                respond(sock, capabilitiesStatus, QByteArray("{\"error\":\"no\"}"));
            } else {
                respond(sock, 200, QJsonDocument(capabilities).toJson(QJsonDocument::Compact));
            }
            return;
        }
        if (req.path == QStringLiteral("/api/v1/sensor")) {
            if (sensorPostStatus != 200) {
                respond(sock, sensorPostStatus, sensorPostError.toUtf8());
            } else {
                respond(sock, 200, QJsonDocument(status).toJson(QJsonDocument::Compact));
            }
            return;
        }
        if (req.path == QStringLiteral("/api/v1/status")) {
            respond(sock, 200, QJsonDocument(status).toJson(QJsonDocument::Compact));
            return;
        }
        if (req.path == QStringLiteral("/api/v1/auth/challenge")) {
            const QJsonObject challenge{
                {QStringLiteral("nonce"),
                 QStringLiteral("00112233445566778899aabbccddeeff")},
                {QStringLiteral("salt"), QStringLiteral("000102030405060708090a0b0c0d0e0f")},
                {QStringLiteral("iterations"), 1000}};
            respond(sock, 200, QJsonDocument(challenge).toJson(QJsonDocument::Compact));
            return;
        }
        if (req.path == QStringLiteral("/api/v1/auth/login")) {
            ++loginRequests;
            // Signing in is what puts the camera back in a state where the
            // write it just rejected will be accepted.
            configUnauthorized = false;
            const QJsonObject session{{QStringLiteral("token"), QStringLiteral("stub-token")},
                                      {QStringLiteral("expires_in_s"), 1800}};
            respond(sock, 200, QJsonDocument(session).toJson(QJsonDocument::Compact));
            return;
        }
        if (req.path.startsWith(QStringLiteral("/api/v1/config"))) {
            ++configRequests;
            if (configUnauthorized) {
                respond(sock, 401, QByteArray("{\"error\":\"unauthorized\"}"));
                return;
            }
            respond(sock, 200, QJsonDocument(status).toJson(QJsonDocument::Compact));
            return;
        }
        respond(sock, 404, QByteArray("{\"error\":\"not found\"}"));
    }

    static QByteArray headerValue(const QByteArray &head, const QByteArray &name)
    {
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray l = line.trimmed();
            if (l.toLower().startsWith(name + ':')) {
                return l.mid(name.size() + 1).trimmed();
            }
        }
        return {};
    }

    static int parseContentLength(const QByteArray &head)
    {
        return headerValue(head, "content-length").toInt();
    }

    static void respond(QTcpSocket *sock, int code, const QByteArray &body)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + " \r\n";
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
        sock->write(out);
        sock->flush();
        sock->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffer;
};

} // namespace

class TestDeviceStatus : public QObject
{
    Q_OBJECT

private slots:
    void capabilitiesArriveAfterAStatusPoll();
    void sensorControlWriteReachesTheSensorEndpoint();
    void unknownControlNeverLeavesTheMachine();
    void rejectedWriteSurfacesTheReasonAndClearsBusy();
    void switchingDevicesDropsTheOldCapabilities();
    void resetAppliesEveryDefaultInOneRequest();
    void writesAreSerialisedWhileOneIsInFlight();
    void aProfileLeavesAsASingleConfigRequest();
    void aStaleNotConnectedErrorClearsWhenTheCameraAnswers();
    void aConfigWriteRejectedWith401IsReplayedAfterSignIn();
    void aConfigWriteRejectedWith401FailsHonestlyWhenSignInCannotStart();
};

void TestDeviceStatus::capabilitiesArriveAfterAStatusPoll()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());

    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    const QJsonObject controls =
        status.capabilities().value(QStringLiteral("controls")).toObject();
    QCOMPARE(controls.size(), 4);
    QVERIFY(controls.contains(QStringLiteral("brightness")));
    QVERIFY(controls.contains(QStringLiteral("hmirror")));
    QCOMPARE(controls.value(QStringLiteral("brightness"))
                 .toObject()
                 .value(QStringLiteral("max"))
                 .toInt(),
             2);

    // Capabilities are only asked for once the camera has answered a status
    // poll, which is the first point at which authentication is known good.
    bool sawStatus = false;
    for (const ControlApiStub::Request &r : stub.requests) {
        if (r.path == QStringLiteral("/api/v1/status"))
            sawStatus = true;
        if (r.path == QStringLiteral("/api/v1/capabilities") && !sawStatus)
            QFAIL("capabilities were requested before any successful status poll");
    }
    status.stopPolling();
}

void TestDeviceStatus::sensorControlWriteReachesTheSensorEndpoint()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    status.setSensorControl(QStringLiteral("brightness"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!status.isSensorBusy(), 8000);
    QVERIFY(status.sensorError().isEmpty());

    ControlApiStub::Request post;
    bool found = false;
    for (const ControlApiStub::Request &r : stub.requests) {
        if (r.method == QStringLiteral("POST") && r.path == QStringLiteral("/api/v1/sensor")) {
            post = r;
            found = true;
        }
    }
    QVERIFY2(found, "no POST reached /api/v1/sensor");

    const QJsonDocument sent = QJsonDocument::fromJson(post.body);
    QVERIFY(sent.isObject());
    QCOMPARE(sent.object().value(QStringLiteral("brightness")).toInt(), 1);
    // Exactly the control that moved, nothing else: a slider drag must not
    // also commit a resolution or a quality change.
    QCOMPARE(sent.object().size(), 1);
    status.stopPolling();
}

void TestDeviceStatus::unknownControlNeverLeavesTheMachine()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    const int postsBefore = [&] {
        int n = 0;
        for (const ControlApiStub::Request &r : stub.requests)
            if (r.method == QStringLiteral("POST"))
                ++n;
        return n;
    }();

    status.setSensorControl(QStringLiteral("not_a_control"), 1);
    QVERIFY(status.sensorError().contains(QStringLiteral("not_a_control")));
    QVERIFY(!status.isSensorBusy());

    // Let any stray request have time to arrive, then prove none did.
    QTest::qWait(500);
    int postsAfter = 0;
    for (const ControlApiStub::Request &r : stub.requests)
        if (r.method == QStringLiteral("POST"))
            ++postsAfter;
    QCOMPARE(postsAfter, postsBefore);
    status.stopPolling();
}

void TestDeviceStatus::rejectedWriteSurfacesTheReasonAndClearsBusy()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());
    stub.sensorPostStatus = 409;
    stub.sensorPostError = QStringLiteral("stream active: disconnect before config change");

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    status.setSensorControl(QStringLiteral("contrast"), -1);
    QTRY_VERIFY_WITH_TIMEOUT(!status.isSensorBusy(), 8000);
    QCOMPARE(status.sensorError(),
             QStringLiteral("stream active: disconnect before config change"));
    status.stopPolling();
}

void TestDeviceStatus::switchingDevicesDropsTheOldCapabilities()
{
    ControlApiStub first;
    QVERIFY(first.listen());
    ControlApiStub second;
    QVERIFY(second.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), first.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!status.capabilities().isEmpty(), 8000);

    status.startPolling(QStringLiteral("127.0.0.1"), second.port());
    // The drop has to be immediate: until the new camera has answered, the
    // panel must not offer controls that describe a different sensor.
    QVERIFY(status.capabilities().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!status.capabilities().isEmpty(), 8000);

    // And the re-fetch must go to the new camera, not be satisfied from cache.
    bool askedSecond = false;
    for (const ControlApiStub::Request &r : second.requests) {
        if (r.path == QStringLiteral("/api/v1/capabilities"))
            askedSecond = true;
    }
    QVERIFY2(askedSecond, "capabilities were not re-requested from the new device");
    status.stopPolling();
}

void TestDeviceStatus::resetAppliesEveryDefaultInOneRequest()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    status.resetSensorControls();
    QTRY_VERIFY_WITH_TIMEOUT(!status.isSensorBusy(), 8000);
    QVERIFY(status.sensorError().isEmpty());

    for (const ControlApiStub::Request &r : stub.requests) {
        if (r.method != QStringLiteral("POST") || r.path != QStringLiteral("/api/v1/sensor"))
            continue;
        const QJsonObject sent = QJsonDocument::fromJson(r.body).object();
        // Three supported controls, and the unsupported colorbar left out even
        // though the payload advertises it.
        QCOMPARE(sent.size(), 3);
        QVERIFY(sent.contains(QStringLiteral("brightness")));
        QVERIFY(sent.contains(QStringLiteral("contrast")));
        QVERIFY(sent.contains(QStringLiteral("hmirror")));
        QVERIFY(!sent.contains(QStringLiteral("colorbar")));
        return;
    }
    QFAIL("no reset POST reached /api/v1/sensor");
}

void TestDeviceStatus::writesAreSerialisedWhileOneIsInFlight()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    // Fire three writes back to back. Only the first may reach the camera:
    // otherwise a drag across several controls would interleave POSTs and the
    // camera would apply them in an order nobody asked for.
    status.setSensorControl(QStringLiteral("brightness"), 1);
    QVERIFY(status.isSensorBusy());
    status.setSensorControl(QStringLiteral("contrast"), 1);
    status.setSensorControl(QStringLiteral("hmirror"), 1);

    QTRY_VERIFY_WITH_TIMEOUT(!status.isSensorBusy(), 8000);
    QTest::qWait(400);

    int posts = 0;
    for (const ControlApiStub::Request &r : stub.requests)
        if (r.method == QStringLiteral("POST"))
            ++posts;
    QCOMPARE(posts, 1);
    status.stopPolling();
}

void TestDeviceStatus::aProfileLeavesAsASingleConfigRequest()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    // A profile is five settings issued in the same tick, which is how the
    // profile selector sends them. The camera takes all of them in one body,
    // and the desktop used to keep a single pending slot that returned early
    // on the second write - so the first setting went out, the other four were
    // discarded in silence, and the camera stayed on whatever it had.
    status.setResolution(QStringLiteral("hd"));
    status.setQuality(12);
    status.setXclk(18);
    status.setFrameBufferCount(3);
    status.setGrabMode(QStringLiteral("latest"));
    QVERIFY(status.configBusy());

    QTRY_VERIFY_WITH_TIMEOUT(!status.configBusy(), 8000);
    QVERIFY2(status.configError().isEmpty(), qPrintable(status.configError()));

    int configRequests = 0;
    QByteArray method;
    QByteArray path;
    QString contentType;
    QJsonObject body;
    for (const ControlApiStub::Request &r : stub.requests) {
        if (r.path.startsWith(QStringLiteral("/api/v1/config"))) {
            ++configRequests;
            method = r.method.toUtf8();
            path = r.path.toUtf8();
            contentType = r.contentType;
            body = QJsonDocument::fromJson(r.body).object();
        }
    }
    QCOMPARE(configRequests, 1);
    // FW-13: a write is a POST whose body carries the configuration, so an
    // exact path is also proof that nothing leaked into a query string (the
    // truncation path FW-10 used to swallow silently).
    QCOMPARE(method, QByteArray("POST"));
    QCOMPARE(path, QByteArray("/api/v1/config"));
    // The content type is the cross-origin contract (ADR-0017): without it a
    // browser can post a simple text/plain request that never preflights.
    QVERIFY2(contentType.contains(QStringLiteral("application/json")),
             qPrintable(contentType));
    QCOMPARE(body.value(QStringLiteral("framesize")).toString(), QStringLiteral("hd"));
    QCOMPARE(body.value(QStringLiteral("quality")).toInt(), 12);
    QCOMPARE(body.value(QStringLiteral("xclk")).toInt(), 18);
    QCOMPARE(body.value(QStringLiteral("fb_count")).toInt(), 3);
    QCOMPARE(body.value(QStringLiteral("grab")).toString(), QStringLiteral("latest"));
    // Numbers stay numbers: a firmware that typed on the JSON type would refuse
    // "12" for quality, so a stringified int is a silently dropped setting.
    QVERIFY(body.value(QStringLiteral("quality")).isDouble());
    QVERIFY(body.value(QStringLiteral("fb_count")).isDouble());
    status.stopPolling();
}

void TestDeviceStatus::aStaleNotConnectedErrorClearsWhenTheCameraAnswers()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    // A write attempted while no camera is known reports "not connected".
    // That sentence has to stop being true the moment the camera answers a
    // status poll - otherwise the card stays on screen for a fault that has
    // already passed, and nobody clears it because nobody is watching for it.
    status.setQuality(12);
    QCOMPARE(status.configError(), QStringLiteral("not connected"));

    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(status.configError().isEmpty(), 8000);

    status.stopPolling();
}

// CP-6: a token that expires mid-session used to end the request with
// "session expired, signing in again" and never send anything again. The batch
// had already been cleared on the GUI side, so the user's change was gone and
// the message promised a retry nothing performed.
void TestDeviceStatus::aConfigWriteRejectedWith401IsReplayedAfterSignIn()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    // A password is what makes a 401 recoverable at all; without one the
    // sign-in declines and there is nothing to wait for.
    status.signIn(QStringLiteral("control-password"), false);
    QTRY_VERIFY_WITH_TIMEOUT(status.isAuthenticated(), 8000);
    const int loginsBefore = stub.loginRequests;
    const int configBefore = stub.configRequests;

    // From here the config endpoint rejects us until a sign-in succeeds - the
    // camera kept the endpoint but not our token.
    stub.configUnauthorized = true;

    status.setResolution(QStringLiteral("hd"));
    status.setQuality(12);
    QVERIFY(status.configBusy());

    QTRY_VERIFY_WITH_TIMEOUT(!status.configBusy(), 15000);
    QVERIFY2(status.configError().isEmpty(), qPrintable(status.configError()));
    // One fresh sign-in, and the batch went out twice: rejected, then accepted.
    QCOMPARE(stub.loginRequests, loginsBefore + 1);
    QCOMPARE(stub.configRequests, configBefore + 2);

    // The replay carries the whole batch, not a fragment of it: a rejected
    // write must arrive as the one request it was originally built as.
    QList<QJsonObject> bodies;
    for (const ControlApiStub::Request &r : stub.requests) {
        if (r.path.startsWith(QStringLiteral("/api/v1/config"))) {
            bodies << QJsonDocument::fromJson(r.body).object();
        }
    }
    QCOMPARE(bodies.size(), 2);
    QVERIFY2(bodies.at(0) == bodies.at(1),
             "the replay was not byte-for-byte the batch the camera rejected");
    for (const QJsonObject &b : bodies) {
        QCOMPARE(b.value(QStringLiteral("framesize")).toString(), QStringLiteral("hd"));
        QCOMPARE(b.value(QStringLiteral("quality")).toInt(), 12);
    }
    status.stopPolling();
}

// The other half of CP-6: when there is no session to get back, the write has
// to fail with a sentence that is true, once, and leaves the panel usable.
void TestDeviceStatus::aConfigWriteRejectedWith401FailsHonestlyWhenSignInCannotStart()
{
    ControlApiStub stub;
    QVERIFY(stub.listen());

    DeviceStatus status;
    QSignalSpy caps(&status, &DeviceStatus::capabilitiesChanged);
    status.startPolling(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(caps.count() >= 1, 8000);

    stub.configUnauthorized = true;

    status.setQuality(12);
    QVERIFY(status.configBusy());

    QTRY_VERIFY_WITH_TIMEOUT(!status.configBusy(), 8000);
    QVERIFY2(!status.configError().isEmpty(), "a dropped write reported success");
    QVERIFY2(status.configError().contains(QStringLiteral("session expired")),
             qPrintable(status.configError()));
    QVERIFY2(!status.configError().contains(QStringLiteral("signing in again")),
             "the message promised a retry that will not happen");

    // One attempt, one honest answer, and the panel is free for the next try.
    QTest::qWait(500);
    QCOMPARE(stub.configRequests, 1);
    QCOMPARE(stub.loginRequests, 0);
    status.stopPolling();
}

QTEST_MAIN(TestDeviceStatus)
#include "tst_devicestatus.moc"
