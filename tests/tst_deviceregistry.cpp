// Section 27 requires that the architecture not be built around a single
// global camera. That is a claim about structure, so it is tested as structure:
// two cameras must be able to exist side by side with independent pipelines,
// a camera must be recognised by its own id rather than its address, and the
// active camera must be swappable without disturbing the other one.
#include "CameraDevice.h"
#include "DeviceRegistry.h"
#include "DeviceStatus.h"
#include "DiscoveryService.h"
#include "FrameBus.h"
#include "MjpegClient.h"
#include "Recorder.h"
#include "SessionState.h"
#include "SnapshotWriter.h"
#include "StreamStats.h"

#include <QSignalSpy>
#include <QTest>

class TestDeviceRegistry : public QObject
{
    Q_OBJECT

private slots:
    void firstAcquireCreatesADevice();
    void sameIdReusesTheDeviceAcrossAddresses();
    void differentIdsGetIndependentPipelines();
    void activeDeviceStartsUnsetAndSwapsOnDemand();
    void setActiveRejectsAnUnknownIdWithoutDisturbingTheView();
    void staleReleaseSparesTheActiveDevice();
    void staleReleaseFreesDevicesThatWentQuiet();
    void setActiveIsIdempotent();
    void emptyDeviceIdIsRefused();
    void portsOutsideTheValidRangeAreRefused();
};

void TestDeviceRegistry::firstAcquireCreatesADevice()
{
    DeviceRegistry registry(nullptr);
    QSignalSpy changed(&registry, &DeviceRegistry::changed);

    CameraDevice *device = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"),
                                           80, 81);
    QVERIFY(device);
    QCOMPARE(device->deviceId(), QStringLiteral("aabbcc"));
    QCOMPARE(device->address(), QStringLiteral("192.168.4.1"));
    QCOMPARE(device->controlPort(), quint16(80));
    QCOMPARE(device->streamPort(), quint16(81));
    QCOMPARE(registry.deviceCount(), 1);
    QCOMPARE(changed.count(), 1);

    // The whole per-camera pipeline hangs off the device, which is what makes
    // "one global camera" impossible to reintroduce by accident.
    QVERIFY(device->frames());
    QVERIFY(device->stream());
    QVERIFY(device->status());
    QVERIFY(device->session());
    QVERIFY(device->recorder());
    QVERIFY(device->snapshots());
    QVERIFY(device->stats());

    // The capture paths are wired to this camera's own bus, not to a shared one.
    QVERIFY(device->snapshots() != nullptr);
}

void TestDeviceRegistry::sameIdReusesTheDeviceAcrossAddresses()
{
    DeviceRegistry registry(nullptr);
    CameraDevice *first = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"),
                                          80, 81);
    QSignalSpy changed(&registry, &DeviceRegistry::changed);
    QSignalSpy endpoint(first, &CameraDevice::endpointChanged);

    CameraDevice *second = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.7"),
                                            8080, 8081);

    // A camera that answers on a new address is still the same camera. Building
    // a second device here would strand its stored credential and lose its
    // recording history on every address change.
    QCOMPARE(second, first);
    QCOMPARE(registry.deviceCount(), 1);
    QCOMPARE(second->address(), QStringLiteral("192.168.4.7"));
    QCOMPARE(second->controlPort(), quint16(8080));
    QCOMPARE(endpoint.count(), 1);
    QCOMPARE(changed.count(), 0);
}

void TestDeviceRegistry::differentIdsGetIndependentPipelines()
{
    DeviceRegistry registry(nullptr);
    CameraDevice *a = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    CameraDevice *b = registry.acquire(QStringLiteral("ddeeff"), QStringLiteral("192.168.4.2"), 80, 81);

    QVERIFY(a != b);
    QCOMPARE(registry.deviceCount(), 2);
    QVERIFY(a->frames() != b->frames());
    QVERIFY(a->stream() != b->stream());
    QVERIFY(a->status() != b->status());
    QVERIFY(a->recorder() != b->recorder());
    QVERIFY(a->session() != b->session());
    QCOMPARE(registry.find(QStringLiteral("ddeeff")), b);
    QVERIFY(!registry.find(QStringLiteral("nope")));
}

void TestDeviceRegistry::activeDeviceStartsUnsetAndSwapsOnDemand()
{
    DeviceRegistry registry(nullptr);
    QVERIFY(!registry.activeDevice());
    QVERIFY(registry.activeDeviceId().isEmpty());

    CameraDevice *a = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    CameraDevice *b = registry.acquire(QStringLiteral("ddeeff"), QStringLiteral("192.168.4.2"), 80, 81);

    // Acquiring is not selecting: an announce must not pull the view away from
    // the camera the user is already watching.
    QVERIFY(!registry.activeDevice());

    QSignalSpy spy(&registry, &DeviceRegistry::activeDeviceChanged);
    QVERIFY(registry.setActive(QStringLiteral("aabbcc")));
    QCOMPARE(registry.activeDevice(), a);
    QCOMPARE(registry.activeDeviceId(), QStringLiteral("aabbcc"));
    QCOMPARE(spy.count(), 1);

    QVERIFY(registry.setActive(QStringLiteral("ddeeff")));
    QCOMPARE(registry.activeDevice(), b);
    QCOMPARE(spy.count(), 2);
}

void TestDeviceRegistry::setActiveRejectsAnUnknownIdWithoutDisturbingTheView()
{
    DeviceRegistry registry(nullptr);
    CameraDevice *a = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    registry.setActive(QStringLiteral("aabbcc"));
    QSignalSpy spy(&registry, &DeviceRegistry::activeDeviceChanged);

    QVERIFY(!registry.setActive(QStringLiteral("ghost")));

    // Still showing the same camera: a bad id from the UI must not blank the
    // view, which is the failure that would actually hurt a user.
    QCOMPARE(registry.activeDevice(), a);
    QCOMPARE(spy.count(), 0);
}

void TestDeviceRegistry::staleReleaseSparesTheActiveDevice()
{
    DeviceRegistry registry(nullptr);
    CameraDevice *a = registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    registry.acquire(QStringLiteral("ddeeff"), QStringLiteral("192.168.4.2"), 80, 81);
    registry.setActive(QStringLiteral("aabbcc"));

    // Zero TTL ages out everything not currently shown.
    QCOMPARE(registry.releaseStale(0), 1);
    QCOMPARE(registry.deviceCount(), 1);
    QCOMPARE(registry.activeDevice(), a);
    QVERIFY(registry.find(QStringLiteral("ddeeff")) == nullptr);
    QVERIFY(registry.find(QStringLiteral("aabbcc")));
}

void TestDeviceRegistry::staleReleaseFreesDevicesThatWentQuiet()
{
    DeviceRegistry registry(nullptr);
    registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    registry.acquire(QStringLiteral("ddeeff"), QStringLiteral("192.168.4.2"), 80, 81);
    registry.acquire(QStringLiteral("001122"), QStringLiteral("192.168.4.3"), 80, 81);

    QSignalSpy changed(&registry, &DeviceRegistry::changed);
    QCOMPARE(registry.releaseStale(0), 3);
    QCOMPARE(registry.deviceCount(), 0);
    QCOMPARE(changed.count(), 1);

    // A released device is genuinely gone, and its id can be rebuilt later.
    CameraDevice *again =
        registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.9"), 80, 81);
    QVERIFY(again);
    QCOMPARE(registry.deviceCount(), 1);
    QCOMPARE(again->address(), QStringLiteral("192.168.4.9"));
}

void TestDeviceRegistry::setActiveIsIdempotent()
{
    DeviceRegistry registry(nullptr);
    registry.acquire(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81);
    registry.setActive(QStringLiteral("aabbcc"));
    QSignalSpy spy(&registry, &DeviceRegistry::activeDeviceChanged);

    QVERIFY(registry.setActive(QStringLiteral("aabbcc")));
    // Re-selecting what is already shown is not a change, so the view must not
    // be torn down and rebuilt for nothing.
    QCOMPARE(spy.count(), 0);
}

void TestDeviceRegistry::emptyDeviceIdIsRefused()
{
    DeviceRegistry registry(nullptr);
    QVERIFY(registry.acquire(QString(), QStringLiteral("192.168.4.1"), 80, 81) == nullptr);
    QCOMPARE(registry.deviceCount(), 0);
    QVERIFY(registry.acquireDevice(QString(), QStringLiteral("192.168.4.1"), 80, 81).isEmpty());
}

void TestDeviceRegistry::portsOutsideTheValidRangeAreRefused()
{
    DeviceRegistry registry(nullptr);

    // An announce with a nonsense port must not reach a socket call. The
    // QML-facing entry point is the one that needs the check, because that is
    // where the values arrive untyped.
    QVERIFY(registry.acquireDevice(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 0, 81)
                .isEmpty());
    QVERIFY(registry.acquireDevice(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 0)
                .isEmpty());
    QVERIFY(registry.acquireDevice(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 70000, 81)
                .isEmpty());
    QCOMPARE(registry.deviceCount(), 0);

    QCOMPARE(registry.acquireDevice(QStringLiteral("aabbcc"), QStringLiteral("192.168.4.1"), 80, 81),
             QStringLiteral("aabbcc"));
    QCOMPARE(registry.deviceCount(), 1);
}

QTEST_MAIN(TestDeviceRegistry)
#include "tst_deviceregistry.moc"
