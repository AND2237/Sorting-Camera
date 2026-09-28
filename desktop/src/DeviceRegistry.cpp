#include "DeviceRegistry.h"

#include "CameraDevice.h"

DeviceRegistry::DeviceRegistry(DiscoveryService *discovery, QObject *parent)
    : QObject(parent)
    , m_discovery(discovery)
{
}

DeviceRegistry::~DeviceRegistry()
{
    qDeleteAll(m_devices);
    m_devices.clear();
    m_byId.clear();
}

CameraDevice *DeviceRegistry::acquire(const QString &deviceId, const QString &address,
                                      quint16 controlPort, quint16 streamPort)
{
    if (deviceId.isEmpty()) {
        return nullptr;
    }

    CameraDevice *device = m_byId.value(deviceId, nullptr);
    if (device) {
        // Same camera, possibly a new endpoint. Nothing is torn down: the
        // stream, the credential and the session state all belong to the
        // camera, not to the address it happened to answer on.
        device->setEndpoint(address, controlPort, streamPort);
        m_seen[device] = QDateTime::currentDateTime();
        return device;
    }

    device = new CameraDevice(deviceId, address, controlPort, streamPort, m_discovery, this);
    m_devices.append(device);
    m_byId.insert(deviceId, device);
    m_seen.insert(device, QDateTime::currentDateTime());
    emit changed();
    return device;
}

CameraDevice *DeviceRegistry::activeDevice() const
{
    return m_active;
}

QString DeviceRegistry::acquireDevice(const QString &deviceId, const QString &address,
                                     int controlPort, int streamPort)
{
    // Ports arrive from QML as numbers and from discovery as quint16; a
    // nonsense value is dropped here rather than passed to a socket.
    if (controlPort <= 0 || controlPort > 65535 || streamPort <= 0 || streamPort > 65535) {
        qWarning("[registry] ignoring announce for %s: port out of range (%d/%d)",
                 qPrintable(deviceId), controlPort, streamPort);
        return QString();
    }
    CameraDevice *device =
        acquire(deviceId, address, quint16(controlPort), quint16(streamPort));
    return device ? device->deviceId() : QString();
}

bool DeviceRegistry::setActiveDevice(const QString &deviceId)
{
    return setActive(deviceId);
}

CameraDevice *DeviceRegistry::find(const QString &deviceId) const
{
    return m_byId.value(deviceId, nullptr);
}

QString DeviceRegistry::activeDeviceId() const
{
    return m_active ? m_active->deviceId() : QString();
}

int DeviceRegistry::deviceCount() const
{
    return m_devices.size();
}

QList<CameraDevice *> DeviceRegistry::devices() const
{
    return m_devices;
}

bool DeviceRegistry::setActive(const QString &deviceId)
{
    CameraDevice *device = m_byId.value(deviceId, nullptr);
    if (!device || device == m_active) {
        return device != nullptr;
    }
    m_active = device;
    m_seen[m_active] = QDateTime::currentDateTime();
    emit activeDeviceChanged();
    return true;
}

int DeviceRegistry::releaseStale(qint64 ttlMs)
{
    const QDateTime cutoff = QDateTime::currentDateTime().addMSecs(-ttlMs);
    int released = 0;
    for (int i = m_devices.size() - 1; i >= 0; --i) {
        CameraDevice *device = m_devices.at(i);
        if (device == m_active) {
            continue;
        }
        const QDateTime seen = m_seen.value(device);
        if (seen.isValid() && seen > cutoff) {
            continue;
        }
        m_byId.remove(device->deviceId());
        m_seen.remove(device);
        m_devices.removeAt(i);
        delete device;
        ++released;
    }
    if (released > 0) {
        emit changed();
    }
    return released;
}
