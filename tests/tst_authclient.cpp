#include "AuthClient.h"

#include <QSignalSpy>
#include <QTest>
#include <QtGlobal>

class TestAuthClient : public QObject
{
    Q_OBJECT

private slots:
    void proofVectorMatchesReference();
    void proofDependsOnNonceAndPassword();
    void tokenIsEmptyBeforeSignIn();
    void signInWithoutPasswordReportsMissingCredential();
    void signInWithoutHostIsIgnored();
    void liveDeviceAcceptsTheCorrectPassword();
    void liveDeviceRejectsTheWrongPassword();
};

void TestAuthClient::proofVectorMatchesReference()
{
    const QByteArray salt = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray nonce = QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const QByteArray password = "test-password-123";

    const QByteArray verifier = AuthClient::deriveVerifier(password, salt, 1000);
    QCOMPARE(verifier.toHex(),
             QByteArray("6596cf1d0f884579e1f2f86646376e78a9786f226928fc251a960824064985fc"));

    const QByteArray proof = AuthClient::computeProof(verifier, nonce);
    QCOMPARE(proof.toHex(),
             QByteArray("d0fcce0b6599c57fe7f30ce82387f1d73e4d7f605f2a85050163c4ecf1dd8323"));
}

void TestAuthClient::proofDependsOnNonceAndPassword()
{
    const QByteArray salt = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray nonce = QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const QByteArray otherNonce = QByteArray::fromHex("ffeeddccbbaa99887766554433221100");

    const QByteArray verifier = AuthClient::deriveVerifier("test-password-123", salt, 1000);
    const QByteArray wrongPassword =
        AuthClient::deriveVerifier("test-password-124", salt, 1000);

    QVERIFY(verifier != wrongPassword);
    QVERIFY(AuthClient::computeProof(verifier, nonce)
            != AuthClient::computeProof(verifier, otherNonce));
    QVERIFY(AuthClient::computeProof(verifier, nonce)
            != AuthClient::computeProof(wrongPassword, nonce));
}

void TestAuthClient::tokenIsEmptyBeforeSignIn()
{
    AuthClient auth;
    QVERIFY(auth.token().isEmpty());
    QVERIFY(!auth.isAuthenticated());
    QVERIFY(!auth.hasPassword());
}

void TestAuthClient::signInWithoutPasswordReportsMissingCredential()
{
    AuthClient auth;
    QSignalSpy settled(&auth, &AuthClient::settled);
    auth.configure("192.0.2.1", 80);
    auth.signIn();
    QCOMPARE(settled.count(), 1);
    QVERIFY(auth.lastError().contains(QStringLiteral("password")));
    QVERIFY(!auth.isAuthenticated());
}

void TestAuthClient::signInWithoutHostIsIgnored()
{
    AuthClient auth;
    auth.setPassword(QStringLiteral("irrelevant"));
    auth.signIn();
    QVERIFY(!auth.isBusy());
    QVERIFY(!auth.isAuthenticated());
}

void TestAuthClient::liveDeviceAcceptsTheCorrectPassword()
{
    const QByteArray host = qgetenv("SCAM_TEST_HOST");
    const QString password = QString::fromLocal8Bit(qgetenv("SCAM_TEST_PASSWORD"));
    if (host.isEmpty() || password.isEmpty()) {
        QSKIP("set SCAM_TEST_HOST and SCAM_TEST_PASSWORD to run the live auth test");
    }

    AuthClient auth;
    QSignalSpy settled(&auth, &AuthClient::settled);
    QSignalSpy authenticated(&auth, &AuthClient::authenticatedChanged);
    auth.configure(QString::fromLocal8Bit(host), 80);
    auth.setPassword(password);
    auth.signIn();

    QTRY_VERIFY_WITH_TIMEOUT(settled.count() > 0 || authenticated.count() > 0, 15000);
    QVERIFY2(auth.isAuthenticated(),
             qPrintable(QStringLiteral("login failed: %1").arg(auth.lastError())));
    QVERIFY(!auth.token().isEmpty());
    QVERIFY(auth.lastError().isEmpty());
}

void TestAuthClient::liveDeviceRejectsTheWrongPassword()
{
    const QByteArray host = qgetenv("SCAM_TEST_HOST");
    if (host.isEmpty() || qgetenv("SCAM_TEST_PASSWORD").isEmpty()) {
        QSKIP("set SCAM_TEST_HOST and SCAM_TEST_PASSWORD to run the live auth test");
    }

    AuthClient auth;
    auth.configure(QString::fromLocal8Bit(host), 80);
    auth.setPassword(QStringLiteral("definitely-not-the-password"));
    auth.signIn();

    QTRY_VERIFY_WITH_TIMEOUT(!auth.lastError().isEmpty(), 15000);
    QVERIFY(!auth.isAuthenticated());
    QVERIFY(auth.lastError().contains(QStringLiteral("invalid proof"),
                                       Qt::CaseInsensitive));
}

QTEST_MAIN(TestAuthClient)
#include "tst_authclient.moc"
