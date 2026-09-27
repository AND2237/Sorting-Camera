#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QtGlobal>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QTimer;

class AuthClient : public QObject
{
    Q_OBJECT

public:
    explicit AuthClient(QObject *parent = nullptr);
    ~AuthClient() override;

    void configure(const QString &host, quint16 port);
    void setPassword(const QString &password);
    void forgetPassword();

    QString host() const;
    quint16 port() const;
    QString token() const;
    bool isAuthenticated() const;
    bool isBusy() const;
    bool hasPassword() const;
    QString lastError() const;
    QString password() const { return m_password; }

    void signIn();
    void invalidate();

    void applyAuthHeaderPublic(QNetworkRequest *request) const;
    static QByteArray deriveVerifier(const QByteArray &password, const QByteArray &salt,
                                     int iterations);
    static QByteArray computeProof(const QByteArray &verifier, const QByteArray &nonce);

signals:
    void authenticatedChanged();
    void busyChanged();
    void lastErrorChanged();
    void settled();

private:
    void abortPending();
    void requestChallenge();
    void submitProof(const QByteArray &nonce);
    void setAuthenticated(bool authenticated);
    void setBusy(bool busy);
    void setLastError(const QString &error);
    static QString messageFromReply(QNetworkReply *reply);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_challengeReply = nullptr;
    QNetworkReply *m_loginReply = nullptr;
    QString m_host;
    quint16 m_port = 80;
    QString m_password;
    QString m_token;
    QByteArray m_pendingSalt;
    int m_pendingIterations = 0;
    QDateTime m_expiresAt;
    QDateTime m_nextAttemptAllowed;
    bool m_authenticated = false;
    bool m_busy = false;
    bool m_challengeOutstanding = false;
    QString m_lastError;
};
