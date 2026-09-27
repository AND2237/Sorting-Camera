#include "CredentialStore.h"

#include <QSettings>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace {

QString settingsKey(const QString &deviceKey)
{
    return QStringLiteral("auth/controlPassword/%1").arg(deviceKey);
}

#ifdef Q_OS_WIN

bool protectBytes(const QByteArray &plain, QByteArray *cipher)
{
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plain.size());
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"SCAM control password", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    *cipher = QByteArray(reinterpret_cast<const char *>(out.pbData),
                         static_cast<int>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool unprotectBytes(const QByteArray &cipher, QByteArray *plain)
{
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(cipher.size());
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    *plain = QByteArray(reinterpret_cast<const char *>(out.pbData),
                        static_cast<int>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

#endif

} // namespace

bool CredentialStore::isPersistent()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool CredentialStore::hasPassword(const QString &deviceKey)
{
    if (deviceKey.isEmpty()) {
        return false;
    }
    QSettings settings;
    return !settings.value(settingsKey(deviceKey)).toString().isEmpty();
}

QString CredentialStore::loadPassword(const QString &deviceKey)
{
    if (deviceKey.isEmpty()) {
        return QString();
    }
    QSettings settings;
    const QString encoded = settings.value(settingsKey(deviceKey)).toString();
    if (encoded.isEmpty()) {
        return QString();
    }

#ifdef Q_OS_WIN
    QByteArray plain;
    if (!unprotectBytes(QByteArray::fromBase64(encoded.toLatin1()), &plain)) {
        qWarning("[auth] stored credential for %s could not be decrypted; sign in again",
                 qPrintable(deviceKey));
        settings.remove(settingsKey(deviceKey));
        return QString();
    }
    const QString password = QString::fromUtf8(plain);
    plain.fill('\0');
    return password;
#else
    qWarning("[auth] credential storage is not available on this platform; sign in again");
    return QString();
#endif
}

bool CredentialStore::savePassword(const QString &deviceKey, const QString &password)
{
    if (deviceKey.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    if (password.isEmpty()) {
        clearPassword(deviceKey);
        return true;
    }
    const QByteArray plain = password.toUtf8();
    QByteArray cipher;
    if (!protectBytes(plain, &cipher)) {
        qWarning("[auth] could not protect the credential for %s", qPrintable(deviceKey));
        return false;
    }
    QSettings settings;
    settings.setValue(settingsKey(deviceKey), QString::fromLatin1(cipher.toBase64()));
    settings.sync();
    return settings.status() == QSettings::NoError;
#else
    Q_UNUSED(deviceKey);
    Q_UNUSED(password);
    return false;
#endif
}

void CredentialStore::clearPassword(const QString &deviceKey)
{
    if (deviceKey.isEmpty()) {
        return;
    }
    QSettings settings;
    settings.remove(settingsKey(deviceKey));
    settings.sync();
}

bool CredentialStore::movePassword(const QString &fromKey, const QString &toKey)
{
    if (fromKey.isEmpty() || toKey.isEmpty() || fromKey == toKey) {
        return false;
    }
    if (!hasPassword(fromKey) || hasPassword(toKey)) {
        return false;
    }
    const bool ok = savePassword(toKey, loadPassword(fromKey));
    if (ok) {
        clearPassword(fromKey);
    }
    return ok;
}
