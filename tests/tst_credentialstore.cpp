#include "CredentialStore.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

class TestCredentialStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void storesAndLoadsPassword();
    void overwritesExistingPassword();
    void clearsPassword();
    void movesPasswordToNewKey();
    void storedValueIsNotPlaintext();
    void emptyKeyIsIgnored();
};

void TestCredentialStore::initTestCase()
{
    // Every QSettings here is default-constructed, which on Windows is
    // NativeFormat - HKCU\Software\SortingCameraTest. Until now these tests
    // wrote real encrypted password blobs into the real per-user registry and
    // "cleaned up" with remove(), which leaves the empty keys behind and still
    // touches the user's machine. Redirect the whole test into a scratch
    // directory before the first QSettings exists (CP-22); DPAPI is
    // unaffected, it operates on bytes, not on where they are stored.
    static QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());

    QCoreApplication::setOrganizationName(QStringLiteral("SortingCameraTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SortingCameraTest"));

    QSettings probe;
    QVERIFY2(probe.fileName().startsWith(dir.path()),
             qPrintable(QStringLiteral("settings escaped the scratch dir: %1")
                            .arg(probe.fileName())));
}

static void purge(const QString &key)
{
    QSettings settings;
    settings.remove(QStringLiteral("auth/controlPassword/%1").arg(key));
    settings.sync();
}

void TestCredentialStore::storesAndLoadsPassword()
{
    const QString key = QStringLiteral("test-device-a");
    purge(key);
    purge(QStringLiteral("192.0.2.10:80"));

    QVERIFY(!CredentialStore::hasPassword(key));
    QVERIFY(CredentialStore::savePassword(key, QStringLiteral("s3cret-pass")));
    QVERIFY(CredentialStore::hasPassword(key));
    QCOMPARE(CredentialStore::loadPassword(key), QStringLiteral("s3cret-pass"));

    purge(key);
}

void TestCredentialStore::overwritesExistingPassword()
{
    const QString key = QStringLiteral("test-device-b");
    purge(key);

    QVERIFY(CredentialStore::savePassword(key, QStringLiteral("first-pass")));
    QVERIFY(CredentialStore::savePassword(key, QStringLiteral("second-pass")));
    QCOMPARE(CredentialStore::loadPassword(key), QStringLiteral("second-pass"));

    purge(key);
}

void TestCredentialStore::clearsPassword()
{
    const QString key = QStringLiteral("test-device-c");
    purge(key);

    QVERIFY(CredentialStore::savePassword(key, QStringLiteral("pass")));
    CredentialStore::clearPassword(key);
    QVERIFY(!CredentialStore::hasPassword(key));
    QVERIFY(CredentialStore::loadPassword(key).isEmpty());
}

void TestCredentialStore::movesPasswordToNewKey()
{
    const QString from = QStringLiteral("192.0.2.55:80");
    const QString to = QStringLiteral("aabbccddeeff");
    purge(from);
    purge(to);

    QVERIFY(CredentialStore::savePassword(from, QStringLiteral("moved-pass")));
    QVERIFY(CredentialStore::movePassword(from, to));
    QVERIFY(!CredentialStore::hasPassword(from));
    QVERIFY(CredentialStore::hasPassword(to));
    QCOMPARE(CredentialStore::loadPassword(to), QStringLiteral("moved-pass"));

    purge(to);
}

void TestCredentialStore::storedValueIsNotPlaintext()
{
    const QString key = QStringLiteral("test-device-d");
    purge(key);

    const QString secret = QStringLiteral("plaintext-must-not-appear");
    QVERIFY(CredentialStore::savePassword(key, secret));

    QSettings settings;
    const QString raw = settings.value(QStringLiteral("auth/controlPassword/%1").arg(key)).toString();
    QVERIFY(!raw.isEmpty());
    QVERIFY2(!raw.contains(secret), "the stored blob must not contain the plaintext password");
    QVERIFY(raw != secret);

    QCOMPARE(CredentialStore::loadPassword(key), secret);

    purge(key);
}

void TestCredentialStore::emptyKeyIsIgnored()
{
    QVERIFY(!CredentialStore::hasPassword(QString()));
    QVERIFY(CredentialStore::loadPassword(QString()).isEmpty());
    QVERIFY(!CredentialStore::savePassword(QString(), QStringLiteral("x")));
    CredentialStore::clearPassword(QString());
    QVERIFY(!CredentialStore::movePassword(QString(), QStringLiteral("y")));
}

QTEST_MAIN(TestCredentialStore)
#include "tst_credentialstore.moc"
