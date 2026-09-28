// User preferences are the layer Master Prompt 37 requires to stay separate
// from build-time, firmware and desktop runtime configuration, so the test
// pins both the defaults and the round-trip. QSettings is redirected into a
// temporary directory first: this test must never touch the settings the real
// application and its stored control password live in.
#include "UserPreferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class TestUserPreferences : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void defaultsMatchTheDocumentedFirstRun();
    void everyFieldRoundTrips();
    void rewritesOnlyWhenTheValueActuallyChanges();
    void valuesSurviveAnIndependentInstance();
    void defaultCaptureDirectoryHandlesMissingPictures();
};

namespace {
const char *kPrefsKeys[] = {
    "preferences/host",          "preferences/controlPort",  "preferences/streamPort",
    "preferences/deviceId",      "preferences/captureDirectory",
    "preferences/framesize",     "preferences/quality",      "preferences/xclkMhz",
    "preferences/frameBufferCount", "preferences/grabMode",
};
} // namespace

void TestUserPreferences::initTestCase()
{
    // Redirect every QSettings the test touches into a scratch directory.
    // Doing it here, before the first QSettings is constructed, is what keeps
    // this test read-write without a single line of code special to it.
    static QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("SortingCameraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_userprefs"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
}

void TestUserPreferences::init()
{
    QSettings s;
    s.clear();
    s.sync();
    QCOMPARE(s.status(), QSettings::NoError);
}

void TestUserPreferences::defaultsMatchTheDocumentedFirstRun()
{
    UserPreferences p;
    QCOMPARE(p.host(), QStringLiteral("192.168.4.1"));
    QCOMPARE(p.controlPort(), 80);
    QCOMPARE(p.streamPort(), 81);
    QVERIFY(p.deviceId().isEmpty());
    QVERIFY(p.captureDirectory().isEmpty());
    QVERIFY(p.framesize().isEmpty());
    QCOMPARE(p.quality(), 12);
    QCOMPARE(p.xclkMhz(), 18);
    QCOMPARE(p.frameBufferCount(), 3);
    QCOMPARE(p.grabMode(), QStringLiteral("latest"));
}

void TestUserPreferences::everyFieldRoundTrips()
{
    UserPreferences p;
    p.setHost(QStringLiteral("10.0.0.7"));
    p.setControlPort(8080);
    p.setStreamPort(8181);
    p.setDeviceId(QStringLiteral("aabbccddeeff"));
    p.setCaptureDirectory(QStringLiteral("D:/captures"));
    p.setFramesize(QStringLiteral("hd"));
    p.setQuality(15);
    p.setXclkMhz(20);
    p.setFrameBufferCount(2);
    p.setGrabMode(QStringLiteral("oldest"));

    UserPreferences fresh;
    QCOMPARE(fresh.host(), QStringLiteral("10.0.0.7"));
    QCOMPARE(fresh.controlPort(), 8080);
    QCOMPARE(fresh.streamPort(), 8181);
    QCOMPARE(fresh.deviceId(), QStringLiteral("aabbccddeeff"));
    QCOMPARE(fresh.captureDirectory(), QStringLiteral("D:/captures"));
    QCOMPARE(fresh.framesize(), QStringLiteral("hd"));
    QCOMPARE(fresh.quality(), 15);
    QCOMPARE(fresh.xclkMhz(), 20);
    QCOMPARE(fresh.frameBufferCount(), 2);
    QCOMPARE(fresh.grabMode(), QStringLiteral("oldest"));
}

void TestUserPreferences::rewritesOnlyWhenTheValueActuallyChanges()
{
    UserPreferences p;
    p.setHost(QStringLiteral("192.168.4.1"));

    QSignalSpy spy(&p, &UserPreferences::changed);
    QVERIFY(spy.isValid());

    // Re-stating the current value is not a change and must not look like one
    // to anything listening, otherwise every round-trip through QML fires a
    // spurious notification.
    p.setHost(QStringLiteral("192.168.4.1"));
    QCOMPARE(spy.count(), 0);

    p.setHost(QStringLiteral("192.168.4.9"));
    QCOMPARE(spy.count(), 1);
}

void TestUserPreferences::valuesSurviveAnIndependentInstance()
{
    {
        UserPreferences writer;
        writer.setHost(QStringLiteral("172.16.0.2"));
        writer.setControlPort(8001);
        writer.setStreamPort(8101);
        writer.setDeviceId(QStringLiteral("deadbeef0001"));
        writer.setCaptureDirectory(QStringLiteral("/tmp/caps"));
        writer.setFramesize(QStringLiteral("vga"));
        writer.setQuality(10);
        writer.setXclkMhz(24);
        writer.setFrameBufferCount(1);
        writer.setGrabMode(QStringLiteral("oldest"));
    }

    QSettings s;
    for (const char *key : kPrefsKeys) {
        QVERIFY2(s.contains(QLatin1String(key)), key);
    }

    UserPreferences reader;
    QCOMPARE(reader.host(), QStringLiteral("172.16.0.2"));
    QCOMPARE(reader.controlPort(), 8001);
    QCOMPARE(reader.streamPort(), 8101);
    QCOMPARE(reader.deviceId(), QStringLiteral("deadbeef0001"));
    QCOMPARE(reader.captureDirectory(), QStringLiteral("/tmp/caps"));
    QCOMPARE(reader.framesize(), QStringLiteral("vga"));
    QCOMPARE(reader.quality(), 10);
    QCOMPARE(reader.xclkMhz(), 24);
    QCOMPARE(reader.frameBufferCount(), 1);
    QCOMPARE(reader.grabMode(), QStringLiteral("oldest"));
}

void TestUserPreferences::defaultCaptureDirectoryHandlesMissingPictures()
{
    // No Pictures location (a machine without one, or a redirected profile
    // that returned nothing) must not produce a path with a stray slash in it.
    QCOMPARE(UserPreferences::defaultCaptureDirectory(QString()),
             QStringLiteral("SortingCamera"));
    QCOMPARE(UserPreferences::defaultCaptureDirectory(QStringLiteral("C:/Users/me/Pictures")),
             QStringLiteral("C:/Users/me/Pictures/SortingCamera"));
}

QTEST_MAIN(TestUserPreferences)
#include "tst_userprefs.moc"
