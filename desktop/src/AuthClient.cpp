#include "AuthClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPasswordDigestor>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kVerifierBytes = 32;
constexpr int kRequestTimeoutMs = 4000;
constexpr int kReauthCooldownMs = 5000;
} // namespace

AuthClient::AuthClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

AuthClient::~AuthClient()
{
    abortPending();
}

QString AuthClient::host() const
{
    return m_host;
}

quint16 AuthClient::port() const
{
    return m_port;
}

QString AuthClient::token() const
{
    if (m_token.isEmpty()) {
        return QString();
    }
    if (m_expiresAt.isValid() && QDateTime::currentDateTimeUtc() >= m_expiresAt) {
        return QString();
    }
    return m_token;
}

bool AuthClient::isAuthenticated() const
{
    return !token().isEmpty();
}

bool AuthClient::isBusy() const
{
    return m_busy;
}

bool AuthClient::hasPassword() const
{
    return !m_password.isEmpty();
}

QString AuthClient::lastError() const
{
    return m_lastError;
}

void AuthClient::configure(const QString &host, quint16 port)
{
    if (m_host == host && m_port == port) {
        return;
    }
    m_host = host;
    m_port = port;
    invalidate();
}

void AuthClient::setPassword(const QString &password)
{
    m_password = password;
    if (password.isEmpty()) {
        invalidate();
    }
}

void AuthClient::forgetPassword()
{
    m_password.clear();
    invalidate();
}

void AuthClient::invalidate()
{
    abortPending();
    m_token.clear();
    m_expiresAt = QDateTime();
    m_challengeOutstanding = false;
    setAuthenticated(false);
}

void AuthClient::applyAuthHeaderPublic(QNetworkRequest *request) const
{
    const QString value = token();
    if (!value.isEmpty()) {
        request->setRawHeader(QByteArrayLiteral("Authorization"),
                              QByteArrayLiteral("Bearer ") + value.toLatin1());
    }
}

QString AuthClient::messageFromReply(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll().trimmed();
    if (!body.isEmpty()) {
        const QString text = QString::fromUtf8(body);
        if (text.startsWith(QLatin1Char('{'))) {
            const QJsonObject obj = QJsonDocument::fromJson(body).object();
            const QString error = obj.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                return error;
            }
        }
        return text.left(200);
    }
    return reply->errorString();
}

QByteArray AuthClient::deriveVerifier(const QByteArray &password, const QByteArray &salt,
                                      int iterations)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password, salt,
                                              iterations, kVerifierBytes);
}

QByteArray AuthClient::computeProof(const QByteArray &verifier, const QByteArray &nonce)
{
    return QMessageAuthenticationCode::hash(nonce, verifier, QCryptographicHash::Sha256);
}

void AuthClient::setAuthenticated(bool authenticated)
{
    if (m_authenticated == authenticated) {
        return;
    }
    m_authenticated = authenticated;
    emit authenticatedChanged();
}

void AuthClient::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void AuthClient::setLastError(const QString &error)
{
    if (m_lastError == error) {
        return;
    }
    m_lastError = error;
    emit lastErrorChanged();
}

void AuthClient::abortPending()
{
    if (m_challengeReply) {
        m_challengeReply->disconnect(this);
        m_challengeReply->abort();
        m_challengeReply->deleteLater();
        m_challengeReply = nullptr;
    }
    if (m_loginReply) {
        m_loginReply->disconnect(this);
        m_loginReply->abort();
        m_loginReply->deleteLater();
        m_loginReply = nullptr;
    }
    setBusy(false);
}

void AuthClient::signIn()
{
    if (m_password.isEmpty() || m_host.isEmpty()) {
        setLastError(tr("no control password available"));
        emit settled();
        return;
    }
    if (m_challengeReply || m_loginReply) {
        return;
    }
    if (QDateTime::currentDateTime() < m_nextAttemptAllowed) {
        return;
    }
    requestChallenge();
}

void AuthClient::requestChallenge()
{
    if (m_host.isEmpty()) {
        return;
    }
    if (!m_challengeOutstanding) {
        setBusy(true);
    }
    m_challengeOutstanding = true;

    const QUrl url(QStringLiteral("http://%1:%2/api/v1/auth/challenge")
                       .arg(m_host)
                       .arg(m_port));
    m_challengeReply = m_nam->get(QNetworkRequest(url));
    QTimer::singleShot(kRequestTimeoutMs, m_challengeReply, &QNetworkReply::abort);
    connect(m_challengeReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_challengeReply;
        m_challengeReply = nullptr;
        if (!reply) {
            return;
        }
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            setBusy(false);
            m_nextAttemptAllowed = QDateTime::currentDateTime()
                                       .addMSecs(kReauthCooldownMs);
            setLastError(tr("no response to the authentication challenge (%1)")
                             .arg(reply->errorString()));
            emit settled();
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString nonceHex = obj.value(QStringLiteral("nonce")).toString();
        const QString saltHex = obj.value(QStringLiteral("salt")).toString();
        const int iterations = obj.value(QStringLiteral("iterations")).toInt(0);
        if (nonceHex.isEmpty() || saltHex.isEmpty() || iterations <= 0) {
            setBusy(false);
            setLastError(tr("the device sent an unusable authentication challenge"));
            emit settled();
            return;
        }
        m_pendingSalt = QByteArray::fromHex(saltHex.toLatin1());
        m_pendingIterations = iterations;
        submitProof(QByteArray::fromHex(nonceHex.toLatin1()));
    });
}

void AuthClient::submitProof(const QByteArray &nonce)
{
    const QByteArray verifier =
        deriveVerifier(m_password.toUtf8(), m_pendingSalt, m_pendingIterations);
    const QByteArray proof = computeProof(verifier, nonce);

    QNetworkRequest request(QUrl(QStringLiteral("http://%1:%2/api/v1/auth/login")
                                     .arg(m_host)
                                     .arg(m_port)));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QJsonObject body{
        {QStringLiteral("nonce"), QString::fromLatin1(nonce.toHex())},
        {QStringLiteral("proof"), QString::fromLatin1(proof.toHex())}};
    m_loginReply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(kRequestTimeoutMs, m_loginReply, &QNetworkReply::abort);
    connect(m_loginReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_loginReply;
        m_loginReply = nullptr;
        if (!reply) {
            return;
        }
        reply->deleteLater();

        m_challengeOutstanding = false;
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            setAuthenticated(false);
            m_nextAttemptAllowed = QDateTime::currentDateTime()
                                       .addMSecs(kReauthCooldownMs);
            setLastError(messageFromReply(reply));
            emit settled();
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString token = obj.value(QStringLiteral("token")).toString();
        if (token.isEmpty()) {
            setAuthenticated(false);
            setLastError(tr("the device did not return a session token"));
            emit settled();
            return;
        }
        m_token = token;
        const int ttl = obj.value(QStringLiteral("expires_in_s")).toInt(1800);
        m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(ttl);
        setLastError(QString());
        setAuthenticated(true);
        qInfo("[auth] authenticated with %s, session valid for %d s", qPrintable(m_host), ttl);
        emit settled();
    });
}
