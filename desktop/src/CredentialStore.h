#pragma once

#include <QString>

class CredentialStore
{
public:
    static bool isPersistent();
    static bool hasPassword(const QString &deviceKey);
    static QString loadPassword(const QString &deviceKey);
    static bool savePassword(const QString &deviceKey, const QString &password);
    static void clearPassword(const QString &deviceKey);
    static bool movePassword(const QString &fromKey, const QString &toKey);
};
