#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class CameraDevice;
class DiscoveryService;

// Master Prompt 27, "future multi-device architectural support". The registry
// is the only place that knows how many cameras exist. It hands out one
// CameraDevice per camera and tracks which one is active; it deliberately holds
// no transport, no decoder and no view, so adding a second window means asking
// for another active device rather than restructuring anything.
//
// Devices are keyed by the camera's own id, never by address, so a camera that
// announces from a new address keeps its session, its credential and its
// recording target.
class DeviceRegistry : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY changed)
    Q_PROPERTY(QString activeDeviceId READ activeDeviceId NOTIFY activeDeviceChanged)

public:
    explicit DeviceRegistry(DiscoveryService *discovery, QObject *parent = nullptr);
    ~DeviceRegistry() override;

    // Returns the device for this id, creating it if this is the first time the
    // camera has been seen. A later call for the same id updates the endpoint
    // instead of building a second device.
    CameraDevice *acquire(const QString &deviceId, const QString &address, quint16 controlPort,
                          quint16 streamPort);

    // QML-facing form of acquire(): returns the device id so the caller can
    // chain it into setActive() without touching a C++ pointer.
    Q_INVOKABLE QString acquireDevice(const QString &deviceId, const QString &address,
                                      int controlPort, int streamPort);
    Q_INVOKABLE bool setActiveDevice(const QString &deviceId);

    CameraDevice *find(const QString &deviceId) const;
    CameraDevice *activeDevice() const;
    QString activeDeviceId() const;
    int deviceCount() const;
    QList<CameraDevice *> devices() const;

    // A false return leaves the active device untouched: an announce for a
    // camera that failed to resolve must not blank the view.
    bool setActive(const QString &deviceId);

    // Destroys every device that has not been seen within ttlMs. The active
    // device is never collected, whatever its age - dropping the camera the
    // user is watching because it went quiet would be a worse failure than
    // keeping one stale object.
    //
    // acquire() calls this before it builds a device for a new id, because
    // that is the only event that grows the registry: cameras are keyed by
    // their own id, so a camera announcing with rotating ids used to build one
    // whole pipeline - an MjpegClient QThread, a DeviceStatus QThread, a
    // NotificationCenter sweep timer - per id and never reclaim any of them.
    // With no production caller the list was monotonic (CP-11).
    int releaseStale(qint64 ttlMs);

    // Default age at which an unselected device becomes eligible for release.
    static constexpr qint64 kStaleTtlMs = 60000;
    // The sweep happens inside acquire(), so the TTL is a knob rather than a
    // constant: it lets the reclaim behaviour be exercised without waiting out
    // a minute of wall clock.
    void setStaleTtlMs(qint64 ttlMs);
    qint64 staleTtlMs() const;

signals:
    // The set of known cameras changed - a new announce created a device.
    void changed();
    // A different camera became the one being shown.
    void activeDeviceChanged();

private:
    DiscoveryService *m_discovery = nullptr;
    QList<CameraDevice *> m_devices;
    QHash<QString, CameraDevice *> m_byId;
    QHash<CameraDevice *, QDateTime> m_seen;
    CameraDevice *m_active = nullptr;
    qint64 m_staleTtlMs = kStaleTtlMs;
};
