#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

class QNetworkDatagram;
class QUdpSocket;
class QTimer;
class QThread;

class DiscoveryWorker;

struct DiscoveredDevice
{
    QString deviceId;
    QString name;
    QString address;
    QString firmware;
    QString sensor;
    int protocolVersion = 0;
    int controlPort = 80;
    int streamPort = 81;
    bool authRequired = true;
    QStringList resolutions;
    QJsonObject controlGroups;
    QDateTime lastSeen;
    int rxBytes = 0;

    bool isValid() const { return !deviceId.isEmpty(); }
};

class DiscoveryService : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    enum Roles {
        DeviceIdRole = Qt::UserRole + 1,
        NameRole,
        AddressRole,
        FirmwareRole,
        SensorRole,
        ProtocolRole,
        AuthRole,
        ResolutionsRole,
        LastSeenRole,
    };

    explicit DiscoveryService(QObject *parent = nullptr);
    ~DiscoveryService() override;

    void setDiscoveryPort(quint16 port);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isScanning() const;
    QString statusText() const;

    Q_INVOKABLE void startScanning();
    Q_INVOKABLE void stopScanning();
    Q_INVOKABLE void queryOnce();
    Q_INVOKABLE int indexOfDevice(const QString &deviceId) const;
    Q_INVOKABLE QJsonObject deviceAt(int row) const;

    bool deviceInfo(const QString &deviceId, DiscoveredDevice *out) const;
    QVector<DiscoveredDevice> devices() const { return m_devices; }

    void ingest(const QByteArray &payload, const QString &senderAddress, qint64 nowMs);
    int prune(qint64 nowMs);

    static bool parseAnnounce(const QByteArray &payload, DiscoveredDevice *out,
                              QString *error);

signals:
    void countChanged();
    void scanningChanged();
    void statusTextChanged();
    void deviceFound(const QString &deviceId, const QString &name, const QString &address);
    void deviceUpdated(const QString &deviceId);
    void deviceLost(const QString &deviceId);
    void queryFinished(int replies);

private:
    int indexOf(const QString &deviceId) const;

    QVector<DiscoveredDevice> m_devices;
    quint16 m_port = 48888;
    QThread *m_thread = nullptr;
    DiscoveryWorker *m_worker = nullptr;
    QTimer *m_pruneTimer = nullptr;
    bool m_scanning = false;
    bool m_listening = false;
    QString m_statusText;
    int m_replies = 0;
};

class DiscoveryWorker : public QObject
{
    Q_OBJECT

public:
    explicit DiscoveryWorker(quint16 port, QObject *parent = nullptr);
    ~DiscoveryWorker() override;

public slots:
    void start();
    void stop();
    void queryOnce();
    void setPort(quint16 port);

signals:
    void datagram(const QByteArray &payload, const QString &senderAddress, qint64 nowMs);
    void listening(bool ok);

private:
    void sendQuery();

    QUdpSocket *m_socket = nullptr;
    QUdpSocket *m_tx = nullptr;
    QTimer *m_timer = nullptr;
    quint16 m_port;
};
