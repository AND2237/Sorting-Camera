#include "DiscoveryService.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QTest>

namespace {

QByteArray announce(const char *deviceId, const char *ip, const char *fw, bool auth = true,
                    int controlPort = 80, int streamPort = 81, int protoVersion = 1)
{
    return QStringLiteral(
        "{\"scam\":1,\"op\":\"announce\","
        "\"device_id\":\"%1\",\"device_name\":\"cam-%1\",\"ip\":\"%2\","
        "\"fw_version\":\"%3\",\"proto_version\":%7,\"sensor\":\"ov2640\","
        "\"control_port\":%4,\"stream_port\":%5,\"auth_required\":%6,"
        "\"resolutions\":[\"qvga\",\"vga\",\"hd\"],"
        "\"controls\":{\"exposure\":[\"aec\",\"aec_value\"]}}")
        .arg(deviceId).arg(ip).arg(fw).arg(controlPort).arg(streamPort)
        .arg(auth ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(protoVersion)
        .toUtf8();
}

} // namespace

class TestDiscovery : public QObject
{
    Q_OBJECT

private slots:
    void parsesAValidAnnouncement();
    void rejectsGarbage();
    void rejectsWrongMagic();
    void rejectsWrongOp();
    void rejectsMissingDeviceId();
    void rejectsAnAnnounceFromAnotherProtocolVersion();
    void defaultsAreAppliedWhenFieldsAreAbsent();
    void deduplicatesByStableDeviceId();
    void detectsChangedAnnouncements();
    void distinguishesTwoDevices();
    void agesOutStaleDevices();
    void keepsFreshDevices();
    void fallbackToSenderAddress();
    void rolesExposeIdentityFields();
    void parsesRealDeviceAnnouncement();
    void liveDeviceIsDiscovered();
    void liveUnicastAndBroadcastBehaviour();
};

void TestDiscovery::parsesAValidAnnouncement()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY2(DiscoveryService::parseAnnounce(announce("aabbcc", "192.168.4.1", "0.2.0"),
                                             &device, &error),
             qPrintable(error));
    QCOMPARE(device.deviceId, QStringLiteral("aabbcc"));
    QCOMPARE(device.name, QStringLiteral("cam-aabbcc"));
    QCOMPARE(device.address, QStringLiteral("192.168.4.1"));
    QCOMPARE(device.firmware, QStringLiteral("0.2.0"));
    QCOMPARE(device.protocolVersion, 1);
    QCOMPARE(device.sensor, QStringLiteral("ov2640"));
    QCOMPARE(device.controlPort, 80);
    QCOMPARE(device.streamPort, 81);
    QVERIFY(device.authRequired);
    QCOMPARE(device.resolutions.size(), 3);
    QVERIFY(device.controlGroups.contains(QStringLiteral("exposure")));
}

void TestDiscovery::rejectsGarbage()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY(!DiscoveryService::parseAnnounce("not json at all", &device, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!DiscoveryService::parseAnnounce("[]", &device, &error));
    QVERIFY(!DiscoveryService::parseAnnounce("", &device, &error));
}

void TestDiscovery::rejectsWrongMagic()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY(!DiscoveryService::parseAnnounce("{\"scam\":2,\"op\":\"announce\","
                                             "\"device_id\":\"x\"}", &device, &error));
    QVERIFY(error.contains(QStringLiteral("scam"), Qt::CaseInsensitive));
}

void TestDiscovery::rejectsWrongOp()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY(!DiscoveryService::parseAnnounce("{\"scam\":1,\"op\":\"query\","
                                             "\"device_id\":\"x\"}", &device, &error));
    QVERIFY(error.contains(QStringLiteral("op"), Qt::CaseInsensitive));
}

void TestDiscovery::rejectsMissingDeviceId()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY(!DiscoveryService::parseAnnounce("{\"scam\":1,\"op\":\"announce\","
                                             "\"ip\":\"192.168.4.1\"}", &device, &error));
    QVERIFY(error.contains(QStringLiteral("device_id")));
}

void TestDiscovery::rejectsAnAnnounceFromAnotherProtocolVersion()
{
    DiscoveryService service;
    QSignalSpy found(&service, &DiscoveryService::deviceFound);
    QSignalSpy status(&service, &DiscoveryService::statusTextChanged);

    // A camera on our version is listed normally.
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), 1000000);
    QCOMPARE(service.rowCount(), 1);
    QCOMPARE(found.count(), 1);

    // The same payload from a camera speaking the next protocol major version
    // must not reach the model: the desktop would otherwise offer it for
    // selection and then drive control endpoints, frame framing and field
    // names that firmware does not implement (CP-21).
    service.ingest(announce("ddeeff", "192.168.4.2", "0.3.0", true, 80, 81, 2),
                   QString(), 1002000);
    QCOMPARE(service.rowCount(), 1);
    QCOMPARE(found.count(), 1);
    QVERIFY(service.indexOfDevice(QStringLiteral("ddeeff")) < 0);

    // The refusal has to be visible, not silent: docs/protocol.md promises a
    // clear UI error naming the mismatch.
    QVERIFY(status.count() > 0);
    QVERIFY(service.statusText().contains(QStringLiteral("ddeeff")));
    QVERIFY(service.statusText().contains(QStringLiteral("version 2")));
    QVERIFY(service.statusText().contains(QStringLiteral("version 1")));

    // An announcement that states no version is unknown, not compatible.
    service.ingest(announce("112233", "192.168.4.3", "0.1.0", true, 80, 81, 0),
                   QString(), 1004000);
    QCOMPARE(service.rowCount(), 1);

    QVERIFY(DiscoveryService::isCompatible(1));
    QVERIFY(!DiscoveryService::isCompatible(2));
    QVERIFY(!DiscoveryService::isCompatible(0));
    QCOMPARE(DiscoveryService::kProtoVersion, 1);

    // The gate sits in ingest(), so the pure parser still reports what the
    // payload said - that separation is what makes the version rule testable.
    DiscoveredDevice parsed;
    QString error;
    QVERIFY(DiscoveryService::parseAnnounce(
        announce("ddeeff", "192.168.4.2", "0.3.0", true, 80, 81, 2), &parsed, &error));
    QCOMPARE(parsed.protocolVersion, 2);
}

void TestDiscovery::defaultsAreAppliedWhenFieldsAreAbsent()
{
    DiscoveredDevice device;
    QString error;
    QVERIFY(DiscoveryService::parseAnnounce("{\"scam\":1,\"op\":\"announce\","
                                            "\"device_id\":\"only-id\"}", &device, &error));
    QCOMPARE(device.deviceId, QStringLiteral("only-id"));
    QCOMPARE(device.name, QStringLiteral("only-id"));
    QCOMPARE(device.controlPort, 80);
    QCOMPARE(device.streamPort, 81);
    QVERIFY(device.authRequired);
    QVERIFY(device.resolutions.isEmpty());
}

void TestDiscovery::deduplicatesByStableDeviceId()
{
    DiscoveryService service;
    QSignalSpy found(&service, &DiscoveryService::deviceFound);

    const qint64 t0 = 1000000;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0);
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0 + 2000);
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0 + 4000);

    QCOMPARE(service.rowCount(), 1);
    QCOMPARE(found.count(), 1);

    DiscoveredDevice device;
    QVERIFY(service.deviceInfo(QStringLiteral("aabbcc"), &device));
    QCOMPARE(device.lastSeen.toMSecsSinceEpoch(), t0 + 4000);
}

void TestDiscovery::detectsChangedAnnouncements()
{
    DiscoveryService service;
    QSignalSpy updated(&service, &DiscoveryService::deviceUpdated);

    const qint64 t0 = 2000000;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0);
    service.ingest(announce("aabbcc", "192.168.4.7", "0.3.0"), QString(), t0 + 1000);

    QCOMPARE(service.rowCount(), 1);
    QCOMPARE(updated.count(), 1);

    DiscoveredDevice device;
    QVERIFY(service.deviceInfo(QStringLiteral("aabbcc"), &device));
    QCOMPARE(device.address, QStringLiteral("192.168.4.7"));
    QCOMPARE(device.firmware, QStringLiteral("0.3.0"));
}

void TestDiscovery::distinguishesTwoDevices()
{
    DiscoveryService service;
    const qint64 t0 = 3000000;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0);
    service.ingest(announce("ddeeff", "192.168.4.2", "0.2.0"), QString(), t0);
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0 + 500);

    QCOMPARE(service.rowCount(), 2);
    QVERIFY(service.indexOfDevice(QStringLiteral("aabbcc")) >= 0);
    QVERIFY(service.indexOfDevice(QStringLiteral("ddeeff")) >= 0);
    QCOMPARE(service.indexOfDevice(QStringLiteral("nope")), -1);
}

void TestDiscovery::agesOutStaleDevices()
{
    DiscoveryService service;
    QSignalSpy lost(&service, &DiscoveryService::deviceLost);

    const qint64 t0 = 4000000;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0);
    service.ingest(announce("ddeeff", "192.168.4.2", "0.2.0"), QString(), t0);

    QCOMPARE(service.prune(t0 + 5000), 0);
    QCOMPARE(service.rowCount(), 2);

    QCOMPARE(service.prune(t0 + 20000), 2);
    QCOMPARE(service.rowCount(), 0);
    QCOMPARE(lost.count(), 2);
}

void TestDiscovery::keepsFreshDevices()
{
    DiscoveryService service;
    const qint64 t0 = 5000000;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0);

    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0"), QString(), t0 + 11000);
    QCOMPARE(service.prune(t0 + 12000), 0);
    QCOMPARE(service.rowCount(), 1);
}

void TestDiscovery::fallbackToSenderAddress()
{
    DiscoveryService service;
    const qint64 t0 = 6000000;
    // A realistic announce: no "ip" field, so ingest() has to fall back to the
    // sender address. It still carries a protocol version, because an announce
    // that omits one is refused rather than assumed compatible (CP-21).
    QByteArray payload =
        "{\"scam\":1,\"op\":\"announce\",\"device_id\":\"noip\",\"proto_version\":1}";
    service.ingest(payload, QStringLiteral("10.0.0.9"), t0);

    DiscoveredDevice device;
    QVERIFY(service.deviceInfo(QStringLiteral("noip"), &device));
    QCOMPARE(device.address, QStringLiteral("10.0.0.9"));
}

void TestDiscovery::rolesExposeIdentityFields()
{
    DiscoveryService service;
    service.ingest(announce("aabbcc", "192.168.4.1", "0.2.0", false), QString(), 7000000);

    const QModelIndex idx = service.index(0, 0);
    QVERIFY(idx.isValid());
    QCOMPARE(service.data(idx, DiscoveryService::DeviceIdRole).toString(),
             QStringLiteral("aabbcc"));
    QCOMPARE(service.data(idx, DiscoveryService::AddressRole).toString(),
             QStringLiteral("192.168.4.1"));
    QCOMPARE(service.data(idx, DiscoveryService::FirmwareRole).toString(),
             QStringLiteral("0.2.0"));
    QCOMPARE(service.data(idx, DiscoveryService::AuthRole).toBool(), false);
    QCOMPARE(service.data(idx, DiscoveryService::ResolutionsRole).toStringList().size(), 3);
    QVERIFY(!service.data(idx, DiscoveryService::NameRole).toString().isEmpty());

    const QModelIndex bad = service.index(5, 0);
    QVERIFY(!service.data(bad, DiscoveryService::DeviceIdRole).isValid());
}

void TestDiscovery::liveDeviceIsDiscovered()
{
    const QByteArray port = qgetenv("SCAM_DISCOVERY_PORT");
    if (port.isEmpty()) {
        QSKIP("set SCAM_DISCOVERY_PORT to run the live discovery test");
    }

    DiscoveryService service;
    service.setDiscoveryPort(static_cast<quint16>(port.toUShort()));
    QSignalSpy found(&service, &DiscoveryService::deviceFound);
    service.startScanning();

    QTRY_VERIFY_WITH_TIMEOUT(found.count() > 0, 15000);
    service.stopScanning();

    const QString deviceId = found.first().at(0).toString();
    const QString name = found.first().at(1).toString();
    const QString address = found.first().at(2).toString();
    QVERIFY(!deviceId.isEmpty());
    QVERIFY(!name.isEmpty());
    QCOMPARE(address, QStringLiteral("192.168.4.1"));

    DiscoveredDevice device;
    QVERIFY(service.deviceInfo(deviceId, &device));
    QCOMPARE(device.protocolVersion, 1);
    QVERIFY(device.authRequired);
    QCOMPARE(device.resolutions.size(), 8);
    QVERIFY(device.controlGroups.contains(QStringLiteral("exposure")));
    qInfo("discovered %s (%s) at %s, fw %s, %lld resolutions", qPrintable(deviceId),
          qPrintable(name), qPrintable(device.address), qPrintable(device.firmware),
          static_cast<long long>(device.resolutions.size()));
}

void TestDiscovery::liveUnicastAndBroadcastBehaviour()
{
    const QByteArray port = qgetenv("SCAM_DISCOVERY_PORT");
    if (port.isEmpty()) {
        QSKIP("set SCAM_DISCOVERY_PORT to run the live discovery test");
    }
    const quint16 discoveryPort = static_cast<quint16>(port.toUShort());
    const QByteArray query("{\"scam\":1,\"op\":\"discover\"}");

    const QStringList targets = {QStringLiteral("192.168.4.1"),
                                 QStringLiteral("192.168.4.255"),
                                 QStringLiteral("255.255.255.255")};
    QMap<QString, bool> reachable;
    for (const QString &target : targets) {
        bool answered = false;
        for (int attempt = 0; attempt < 4 && !answered; attempt++) {
            QUdpSocket socket;
            QHostAddress address(target);
            if (!socket.bind(QHostAddress::AnyIPv4, discoveryPort + 1,
                             QAbstractSocket::ShareAddress)) {
                break;
            }
            QObject::connect(&socket, &QUdpSocket::readyRead, &socket, [&]() {
                if (socket.hasPendingDatagrams()) {
                    socket.receiveDatagram();
                    answered = true;
                }
            });
            socket.writeDatagram(query, address, discoveryPort);
            const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 1200;
            while (!answered && QDateTime::currentMSecsSinceEpoch() < deadline) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(20);
            }
            if (!answered) {
                QThread::msleep(400);
            }
        }
        reachable.insert(target, answered);
        qInfo("discovery target %-16s answered=%s", qPrintable(target),
              answered ? "yes" : "no");
    }

    QVERIFY2(reachable.value(QStringLiteral("192.168.4.1")),
             "the device must answer a unicast discovery query");
    qInfo("summary: unicast=%s subnet-broadcast=%s limited-broadcast=%s",
          reachable.value(QStringLiteral("192.168.4.1")) ? "yes" : "no",
          reachable.value(QStringLiteral("192.168.4.255")) ? "yes" : "no",
          reachable.value(QStringLiteral("255.255.255.255")) ? "yes" : "no");
}

void TestDiscovery::parsesRealDeviceAnnouncement()
{
    DiscoveredDevice device;
    QString error;
    const QByteArray payload = R"SCAM({"scam":1,"op":"announce","device_id":"b4bfe9343ae0","device_name":"sorting-cam-01","ip":"192.168.4.1","fw_version":"v0.1.0-2-g42c786e-dirty","proto_version":1,"sensor":"ov2640","control_port":80,"stream_port":81,"auth_required":true,"resolutions":["qqvga","qvga","vga","svga","xga","hd","sxga","uxga"],"controls":{"image":["brightness"],"image":["contrast"],"image":["saturation"],"image":["ae_level"],"image":["special_effect"],"white_balance":["wb_mode"],"exposure":["aec_value"],"gain":["agc_gain"],"gain":["gainceiling"],"gain":["agc"],"exposure":["aec"],"exposure":["aec2"],"white_balance":["awb"],"white_balance":["awb_gain"],"orientation":["hmirror"],"orientation":["vflip"],"processing":["bpc"],"processing":["wpc"],"processing":["raw_gma"],"processing":["lenc"],"processing":["dcw"],"diagnostic":["colorbar"]}})SCAM";
    QVERIFY2(DiscoveryService::parseAnnounce(payload, &device, &error),
             qPrintable(error));
    QCOMPARE(device.deviceId, QStringLiteral("b4bfe9343ae0"));
    QCOMPARE(device.address, QStringLiteral("192.168.4.1"));
    QCOMPARE(device.name, QStringLiteral("sorting-cam-01"));
    QCOMPARE(device.resolutions.size(), 8);
    QVERIFY(device.authRequired);
    QCOMPARE(device.controlGroups.size(), 7);
    qInfo("groups=%d image has %lld entries, exposure has %lld",
          device.controlGroups.size(),
          static_cast<long long>(device.controlGroups.value(QStringLiteral("image")).toArray().size()),
          static_cast<long long>(device.controlGroups.value(QStringLiteral("exposure")).toArray().size()));
}
QTEST_MAIN(TestDiscovery)
#include "tst_discovery.moc"
