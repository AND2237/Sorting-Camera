#pragma once

#include <QObject>
#include <QString>

// Master Prompt 37. User preferences are one of four configuration layers and
// are kept apart from build-time config, firmware runtime config and desktop
// runtime config. Everything here is a choice the user made, persisted to
// QSettings; nothing here is a credential - passwords live in CredentialStore
// and are protected with DPAPI, never here.
class UserPreferences : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString host READ host WRITE setHost NOTIFY changed)
    Q_PROPERTY(int controlPort READ controlPort WRITE setControlPort NOTIFY changed)
    Q_PROPERTY(int streamPort READ streamPort WRITE setStreamPort NOTIFY changed)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY changed)
    Q_PROPERTY(QString captureDirectory READ captureDirectory WRITE setCaptureDirectory NOTIFY changed)
    Q_PROPERTY(QString framesize READ framesize WRITE setFramesize NOTIFY changed)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY changed)
    Q_PROPERTY(int xclkMhz READ xclkMhz WRITE setXclkMhz NOTIFY changed)
    Q_PROPERTY(int frameBufferCount READ frameBufferCount WRITE setFrameBufferCount NOTIFY changed)
    Q_PROPERTY(QString grabMode READ grabMode WRITE setGrabMode NOTIFY changed)
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY changed)

public:
    explicit UserPreferences(QObject *parent = nullptr);

    QString host() const;
    void setHost(const QString &host);
    int controlPort() const;
    void setControlPort(int port);
    int streamPort() const;
    void setStreamPort(int port);
    QString deviceId() const;
    void setDeviceId(const QString &id);
    QString captureDirectory() const;
    void setCaptureDirectory(const QString &dir);
    QString framesize() const;
    void setFramesize(const QString &key);
    int quality() const;
    void setQuality(int quality);
    int xclkMhz() const;
    void setXclkMhz(int mhz);
    int frameBufferCount() const;
    void setFrameBufferCount(int count);
    QString grabMode() const;
    void setGrabMode(const QString &mode);
    // Section 25's dark/light readiness. Persisted as a choice like any other
    // preference, so the application comes up the way the user left it rather
    // than snapping back to the build's default.
    QString themeMode() const;
    void setThemeMode(const QString &mode);

    // Sensible defaults used on first run; kept here rather than in QML so
    // there is one place that defines what "unset" means.
    static QString defaultHost();
    static QString defaultCaptureDirectory(const QString &picturesLocation);

signals:
    void changed();

private:
    void setValue(const char *key, const QVariant &value);
    QVariant value(const char *key, const QVariant &fallback) const;
};
