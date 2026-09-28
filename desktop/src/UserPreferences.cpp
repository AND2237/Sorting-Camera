#include "UserPreferences.h"

#include <QSettings>
#include <QVariant>

namespace {
constexpr int kDefaultControlPort = 80;
constexpr int kDefaultStreamPort = 81;
constexpr int kDefaultQuality = 12;
constexpr int kDefaultXclkMhz = 18;
constexpr int kDefaultFbCount = 3;
} // namespace

UserPreferences::UserPreferences(QObject *parent) : QObject(parent) {}

QVariant UserPreferences::value(const char *key, const QVariant &fallback) const
{
    QSettings s;
    const QVariant v = s.value(QLatin1String(key), fallback);
    return v.isValid() ? v : fallback;
}

void UserPreferences::setValue(const char *key, const QVariant &v)
{
    QSettings s;
    if (s.value(QLatin1String(key)) == v) {
        return;
    }
    s.setValue(QLatin1String(key), v);
    s.sync();
    emit changed();
}

QString UserPreferences::defaultHost()
{
    // The softAP address from ADR-0006 is a sensible first-run default, but it
    // is only ever a default: discovery and the restored preference both
    // outrank it once anything is known.
    return QStringLiteral("192.168.4.1");
}

QString UserPreferences::defaultCaptureDirectory(const QString &picturesLocation)
{
    if (picturesLocation.isEmpty()) {
        return QStringLiteral("SortingCamera");
    }
    return picturesLocation + QStringLiteral("/SortingCamera");
}

QString UserPreferences::host() const
{
    return value("preferences/host", defaultHost()).toString();
}

void UserPreferences::setHost(const QString &host)
{
    setValue("preferences/host", host);
}

int UserPreferences::controlPort() const
{
    return value("preferences/controlPort", kDefaultControlPort).toInt();
}

void UserPreferences::setControlPort(int port)
{
    setValue("preferences/controlPort", port);
}

int UserPreferences::streamPort() const
{
    return value("preferences/streamPort", kDefaultStreamPort).toInt();
}

void UserPreferences::setStreamPort(int port)
{
    setValue("preferences/streamPort", port);
}

QString UserPreferences::deviceId() const
{
    return value("preferences/deviceId", QString()).toString();
}

void UserPreferences::setDeviceId(const QString &id)
{
    setValue("preferences/deviceId", id);
}

QString UserPreferences::captureDirectory() const
{
    return value("preferences/captureDirectory", QString()).toString();
}

void UserPreferences::setCaptureDirectory(const QString &dir)
{
    setValue("preferences/captureDirectory", dir);
}

QString UserPreferences::framesize() const
{
    return value("preferences/framesize", QString()).toString();
}

void UserPreferences::setFramesize(const QString &key)
{
    setValue("preferences/framesize", key);
}

int UserPreferences::quality() const
{
    return value("preferences/quality", kDefaultQuality).toInt();
}

void UserPreferences::setQuality(int quality)
{
    setValue("preferences/quality", quality);
}

int UserPreferences::xclkMhz() const
{
    return value("preferences/xclkMhz", kDefaultXclkMhz).toInt();
}

void UserPreferences::setXclkMhz(int mhz)
{
    setValue("preferences/xclkMhz", mhz);
}

int UserPreferences::frameBufferCount() const
{
    return value("preferences/frameBufferCount", kDefaultFbCount).toInt();
}

void UserPreferences::setFrameBufferCount(int count)
{
    setValue("preferences/frameBufferCount", count);
}

QString UserPreferences::grabMode() const
{
    return value("preferences/grabMode", QStringLiteral("latest")).toString();
}

void UserPreferences::setGrabMode(const QString &mode)
{
    setValue("preferences/grabMode", mode);
}
