#include "DiscoveryService.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

namespace {
constexpr int kScanIntervalMs = 2000;
constexpr qint64 kDeviceTimeoutMs = 12000;
const char *kQuery = "{\"scam\":1,\"op\":\"discover\"}";
} // namespace

DiscoveryService::DiscoveryService(QObject *parent)
    : QAbstractListModel(parent)
    , m_thread(new QThread(this))
    , m_worker(new DiscoveryWorker(m_port))
    , m_statusText(QStringLiteral("idle"))
{
    m_worker->moveToThread(m_thread);
    m_thread->setObjectName(QStringLiteral("discovery"));

    connect(m_worker, &DiscoveryWorker::datagram, this,
            [this](const QByteArray &payload, const QString &senderAddress, qint64 nowMs) {
                const int before = m_devices.size();
                ingest(payload, senderAddress, nowMs);
                if (m_devices.size() != before && m_statusText == QStringLiteral("scanning")) {
                    m_statusText = QStringLiteral("%1 camera(s) found").arg(m_devices.size());
                    emit statusTextChanged();
                }
            }, Qt::QueuedConnection);

    connect(m_worker, &DiscoveryWorker::listening, this, [this](bool ok) {
        m_listening = ok;
        if (!ok && m_scanning) {
            m_statusText = QStringLiteral("discovery port %1 unavailable").arg(m_port);
            emit statusTextChanged();
        }
    }, Qt::QueuedConnection);

    m_pruneTimer = new QTimer(this);
    m_pruneTimer->setInterval(1000);
    connect(m_pruneTimer, &QTimer::timeout, this, [this]() {
        prune(QDateTime::currentMSecsSinceEpoch());
    });

    m_thread->start();
}

DiscoveryService::~DiscoveryService()
{

    disconnect(m_worker, nullptr, this, nullptr);
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    }

    m_thread->quit();
    m_thread->wait();

    delete m_worker;
    m_worker = nullptr;

}

void DiscoveryService::setDiscoveryPort(quint16 port)
{
    if (port == 0 || port == m_port) {
        return;
    }
    m_port = port;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "setPort", Qt::QueuedConnection,
                                  Q_ARG(quint16, port));
    }
}

int DiscoveryService::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_devices.size();
}

QVariant DiscoveryService::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_devices.size()) {
        return QVariant();
    }
    const DiscoveredDevice &device = m_devices.at(index.row());
    switch (role) {
    case DeviceIdRole: return device.deviceId;
    case NameRole: return device.name;
    case AddressRole: return device.address;
    case FirmwareRole: return device.firmware;
    case SensorRole: return device.sensor;
    case ProtocolRole: return device.protocolVersion;
    case AuthRole: return device.authRequired;
    case ResolutionsRole: return device.resolutions;
    case LastSeenRole: return device.lastSeen;
    default: return QVariant();
    }
}

QHash<int, QByteArray> DiscoveryService::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[DeviceIdRole] = "deviceId";
    roles[NameRole] = "name";
    roles[AddressRole] = "address";
    roles[FirmwareRole] = "firmware";
    roles[SensorRole] = "sensor";
    roles[ProtocolRole] = "protocolVersion";
    roles[AuthRole] = "authRequired";
    roles[ResolutionsRole] = "resolutions";
    roles[LastSeenRole] = "lastSeen";
    return roles;
}

bool DiscoveryService::isScanning() const
{
    return m_scanning;
}

QString DiscoveryService::statusText() const
{
    return m_statusText;
}

int DiscoveryService::indexOf(const QString &deviceId) const
{
    for (int i = 0; i < m_devices.size(); i++) {
        if (m_devices.at(i).deviceId == deviceId) {
            return i;
        }
    }
    return -1;
}

int DiscoveryService::indexOfDevice(const QString &deviceId) const
{
    return indexOf(deviceId);
}

QJsonObject DiscoveryService::deviceAt(int row) const
{
    QJsonObject out;
    if (row < 0 || row >= m_devices.size()) {
        return out;
    }
    const DiscoveredDevice &d = m_devices.at(row);
    out["deviceId"] = d.deviceId;
    out["name"] = d.name;
    out["address"] = d.address;
    out["firmware"] = d.firmware;
    out["sensor"] = d.sensor;
    out["protocolVersion"] = d.protocolVersion;
    out["controlPort"] = d.controlPort;
    out["streamPort"] = d.streamPort;
    out["authRequired"] = d.authRequired;
    out["lastSeen"] = d.lastSeen.toString(Qt::ISODate);
    QJsonArray res;
    for (const QString &r : d.resolutions) {
        res.append(r);
    }
    out["resolutions"] = res;
    out["controlGroups"] = d.controlGroups;
    return out;
}

bool DiscoveryService::deviceInfo(const QString &deviceId, DiscoveredDevice *out) const
{
    const int row = indexOf(deviceId);
    if (row < 0) {
        return false;
    }
    if (out) {
        *out = m_devices.at(row);
    }
    return true;
}

bool DiscoveryService::parseAnnounce(const QByteArray &payload, DiscoveredDevice *out,
                                     QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
        }
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("scam")).toInt() != 1) {
        if (error) {
            *error = QStringLiteral("not a scam announcement");
        }
        return false;
    }
    if (root.value(QStringLiteral("op")).toString() != QLatin1String("announce")) {
        if (error) {
            *error = QStringLiteral("unexpected op");
        }
        return false;
    }
    const QString deviceId = root.value(QStringLiteral("device_id")).toString();
    if (deviceId.isEmpty()) {
        if (error) {
            *error = QStringLiteral("missing device_id");
        }
        return false;
    }

    DiscoveredDevice device;
    device.deviceId = deviceId;
    device.name = root.value(QStringLiteral("device_name")).toString(deviceId);
    device.address = root.value(QStringLiteral("ip")).toString();
    device.firmware = root.value(QStringLiteral("fw_version")).toString();
    device.sensor = root.value(QStringLiteral("sensor")).toString();
    device.protocolVersion = root.value(QStringLiteral("proto_version")).toInt(0);
    device.controlPort = root.value(QStringLiteral("control_port")).toInt(80);
    device.streamPort = root.value(QStringLiteral("stream_port")).toInt(81);
    device.authRequired = root.value(QStringLiteral("auth_required")).toBool(true);
    const QJsonArray resolutions = root.value(QStringLiteral("resolutions")).toArray();
    for (const QJsonValue &value : resolutions) {
        device.resolutions << value.toString();
    }
    device.controlGroups = root.value(QStringLiteral("controls")).toObject();
    device.rxBytes = payload.size();

    if (out) {
        *out = device;
    }
    return true;
}

void DiscoveryService::ingest(const QByteArray &payload, const QString &senderAddress, qint64 nowMs)
{
    DiscoveredDevice parsed;
    QString error;
    if (!parseAnnounce(payload, &parsed, &error)) {
        return;
    }
    if (parsed.address.isEmpty() && !senderAddress.isEmpty()) {
        parsed.address = senderAddress;
    }
    parsed.lastSeen = QDateTime::fromMSecsSinceEpoch(nowMs);

    const int row = indexOf(parsed.deviceId);
    if (row < 0) {
        beginInsertRows(QModelIndex(), m_devices.size(), m_devices.size());
        m_devices.append(parsed);
        endInsertRows();
        emit countChanged();
        emit deviceFound(parsed.deviceId, parsed.name, parsed.address);
        return;
    }

    const DiscoveredDevice previous = m_devices.at(row);
    m_devices[row] = parsed;
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed);
    if (previous.address != parsed.address || previous.firmware != parsed.firmware
        || previous.authRequired != parsed.authRequired
        || previous.controlPort != parsed.controlPort
        || previous.streamPort != parsed.streamPort
        || previous.resolutions != parsed.resolutions) {
        emit deviceUpdated(parsed.deviceId);
    }
    ++m_replies;
}

int DiscoveryService::prune(qint64 nowMs)
{
    int removed = 0;
    for (int i = m_devices.size() - 1; i >= 0; i--) {
        const qint64 age = nowMs - m_devices.at(i).lastSeen.toMSecsSinceEpoch();
        if (age > kDeviceTimeoutMs) {
            const QString deviceId = m_devices.at(i).deviceId;
            beginRemoveRows(QModelIndex(), i, i);
            m_devices.remove(i);
            endRemoveRows();
            emit deviceLost(deviceId);
            ++removed;
        }
    }
    if (removed > 0) {
        emit countChanged();
    }
    return removed;
}

void DiscoveryService::startScanning()
{
    if (m_scanning) {
        return;
    }
    m_scanning = true;
    m_replies = 0;
    m_statusText = QStringLiteral("scanning");
    emit scanningChanged();
    emit statusTextChanged();
    m_pruneTimer->start();
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void DiscoveryService::stopScanning()
{
    if (!m_scanning) {
        return;
    }
    m_scanning = false;
    m_statusText = QStringLiteral("idle");
    m_pruneTimer->stop();
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
    emit scanningChanged();
    emit statusTextChanged();
}

void DiscoveryService::queryOnce()
{
    if (!m_scanning) {
        startScanning();
        return;
    }
    QMetaObject::invokeMethod(m_worker, "queryOnce", Qt::QueuedConnection);
}

DiscoveryWorker::DiscoveryWorker(quint16 port, QObject *parent)
    : QObject(parent)
    , m_port(port)
{
}

DiscoveryWorker::~DiscoveryWorker()
{
    if (m_socket) {
        m_socket->close();
    }
}

void DiscoveryWorker::start()
{
    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        m_tx = m_socket;
        connect(m_socket, &QUdpSocket::readyRead, this, [this]() {
            while (m_socket->hasPendingDatagrams()) {
                const QNetworkDatagram packet = m_socket->receiveDatagram();
                emit datagram(packet.data(), packet.senderAddress().toString(),
                              QDateTime::currentMSecsSinceEpoch());
            }
        });
    }
    const bool bound = m_socket->bind(QHostAddress::AnyIPv4, m_port,
                                      QAbstractSocket::ShareAddress);
    if (!bound) {
        qWarning("[discovery] could not bind UDP %u: %s", m_port,
                 qPrintable(m_socket->errorString()));
    } else {
        qInfo("[discovery] listening on UDP %u", m_port);
    }
    emit listening(bound);

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(kScanIntervalMs);
        connect(m_timer, &QTimer::timeout, this, &DiscoveryWorker::sendQuery);
    }
    m_timer->start();
    sendQuery();
}

void DiscoveryWorker::stop()
{
    if (m_timer) {
        m_timer->stop();
    }
}

void DiscoveryWorker::queryOnce()
{
    sendQuery();
}

void DiscoveryWorker::setPort(quint16 port)
{
    if (port == 0 || port == m_port) {
        return;
    }
    const bool wasBound = (m_socket && m_socket->state() == QAbstractSocket::BoundState);
    m_port = port;
    if (wasBound) {
        m_socket->close();
        m_socket->bind(QHostAddress::AnyIPv4, m_port, QAbstractSocket::ShareAddress);
    }
}

void DiscoveryWorker::sendQuery()
{
    if (!m_tx) {
        return;
    }
    const QByteArray payload(kQuery);
    const quint16 port = m_port;

    QList<QHostAddress> targets;
    QSet<QHostAddress> seen;
    targets << QHostAddress(QHostAddress::Broadcast);
    seen.insert(QHostAddress(QHostAddress::Broadcast));
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)
            || !(interface.flags() & QNetworkInterface::IsRunning)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const quint32 ip = address.toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (mask == 0) {
                continue;
            }
            const QHostAddress broadcast((ip & mask) | ~mask);
            if (!seen.contains(broadcast)) {
                seen.insert(broadcast);
                targets << broadcast;
            }
        }
    }

    for (const QHostAddress &target : targets) {
        const qint64 written = m_tx->writeDatagram(payload, target, port);
        if (written != payload.size()) {
            qWarning("[discovery] write to %s failed: %s", qPrintable(target.toString()),
                     qPrintable(m_tx->errorString()));
        }
    }
}
