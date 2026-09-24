#include "MjpegClient.h"

#include <QObject>
#include <QTcpSocket>
#include <QThread>

namespace {
constexpr qint64 kMaxBufferBytes = 8 * 1024 * 1024;
const QByteArray kBoundary = "--FRAME";
} // namespace

class MjpegWorker : public QObject
{
    Q_OBJECT

public:
    explicit MjpegWorker(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~MjpegWorker() override
    {
        closeSocket();
    }

public slots:
    void start(const QString &host, quint16 port)
    {
        if (m_socket) {
            return;
        }
        resetParser();
        m_error.clear();
        emit errorUpdated(QString());

        m_socket = new QTcpSocket(this);
        const QString hostName = host;
        connect(m_socket, &QTcpSocket::connected, this, [this, hostName]() {
            m_active = true;
            emit activeUpdated(true);
            const QByteArray req = "GET /stream HTTP/1.1\r\nHost: " + hostName.toUtf8()
                                   + "\r\nConnection: close\r\n\r\n";
            m_socket->write(req);
        });
        connect(m_socket, &QTcpSocket::readyRead, this, &MjpegWorker::onReadyRead);
        connect(m_socket, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) {
                    fail(m_socket ? m_socket->errorString() : QStringLiteral("socket error"));
                });
        connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
            if (m_active) {
                m_active = false;
                emit activeUpdated(false);
            }
            closeSocket();
        });

        m_socket->connectToHost(host, port);
    }

    void stop()
    {
        if (m_socket) {
            QTcpSocket *sock = m_socket;
            m_socket = nullptr;
            sock->disconnect(this);
            sock->abort();
            sock->deleteLater();
            resetParser();
        }
        if (m_active) {
            m_active = false;
            emit activeUpdated(false);
        }
    }

signals:
    void activeUpdated(bool active);
    void statsUpdated(quint32 framesReceived, quint32 framesDropped, qint64 bytesReceived);
    void errorUpdated(const QString &errorString);
    void frameReady(const QImage &image);

private:
    enum class HttpState { RespHeaders, Identity, ChunkSize, ChunkData, ChunkEnd, Done };
    enum class ParseState { Boundary, Headers, Body };

    void resetParser()
    {
        m_httpState = HttpState::RespHeaders;
        m_parseState = ParseState::Boundary;
        m_contentLength = 0;
        m_chunkRemaining = 0;
        m_chunked = false;
        m_buffer.clear();
        m_raw.clear();
    }

    void closeSocket()
    {
        if (!m_socket) {
            return;
        }
        m_socket->deleteLater();
        m_socket = nullptr;
        resetParser();
    }

    void fail(const QString &reason)
    {
        m_error = reason;
        emit errorUpdated(reason);
        if (m_socket) {
            QTcpSocket *sock = m_socket;
            m_socket = nullptr;
            sock->disconnect(this);
            sock->abort();
            sock->deleteLater();
            resetParser();
        }
        if (m_active) {
            m_active = false;
            emit activeUpdated(false);
        }
    }

    void onReadyRead()
    {
        if (!m_socket) {
            return;
        }
        const QByteArray chunk = m_socket->readAll();
        m_bytes += chunk.size();
        m_raw.append(chunk);

        if (m_buffer.size() + m_raw.size() > kMaxBufferBytes) {
            fail(QStringLiteral("stream buffer overflow (%1 bytes)")
                     .arg(m_buffer.size() + m_raw.size()));
            return;
        }

        pump();

        emit statsUpdated(m_received, m_dropped, m_bytes);
    }

    void feedDecoded(const QByteArray &data)
    {
        if (data.isEmpty()) {
            return;
        }
        m_buffer.append(data);
        while (processBuffer()) {
        }
    }

    void pump()
    {
        while (true) {
            switch (m_httpState) {
            case HttpState::RespHeaders: {
                const int idx = m_raw.indexOf("\r\n\r\n");
                if (idx < 0) {
                    if (m_raw.size() > 8192) {
                        fail(QStringLiteral("malformed HTTP response headers"));
                    }
                    return;
                }
                const QByteArray hdrs = m_raw.left(idx).toLower();
                m_chunked = hdrs.contains("transfer-encoding") && hdrs.contains("chunked");
                m_raw.remove(0, idx + 4);
                m_httpState = m_chunked ? HttpState::ChunkSize : HttpState::Identity;
                break;
            }
            case HttpState::Identity: {
                feedDecoded(m_raw);
                m_raw.clear();
                return;
            }
            case HttpState::ChunkSize: {
                const int idx = m_raw.indexOf("\r\n");
                if (idx < 0) {
                    if (m_raw.size() > 128) {
                        fail(QStringLiteral("malformed chunk size line"));
                    }
                    return;
                }
                QByteArray line = m_raw.left(idx);
                const int semi = line.indexOf(';');
                if (semi >= 0) {
                    line = line.left(semi);
                }
                bool ok = false;
                const int size = line.trimmed().toInt(&ok, 16);
                if (!ok || size < 0) {
                    fail(QStringLiteral("bad chunk size: %1").arg(QString::fromUtf8(line)));
                    return;
                }
                m_raw.remove(0, idx + 2);
                if (size == 0) {
                    m_httpState = HttpState::Done;
                    return;
                }
                m_chunkRemaining = size;
                m_httpState = HttpState::ChunkData;
                break;
            }
            case HttpState::ChunkData: {
                if (m_raw.isEmpty()) {
                    return;
                }
                const int take = qMin(m_chunkRemaining, int(m_raw.size()));
                feedDecoded(m_raw.left(take));
                m_raw.remove(0, take);
                m_chunkRemaining -= take;
                if (m_chunkRemaining > 0) {
                    return;
                }
                m_httpState = HttpState::ChunkEnd;
                break;
            }
            case HttpState::ChunkEnd: {
                if (m_raw.size() < 2) {
                    return;
                }
                if (m_raw[0] != '\r' || m_raw[1] != '\n') {
                    fail(QStringLiteral("malformed chunk terminator"));
                    return;
                }
                m_raw.remove(0, 2);
                m_httpState = HttpState::ChunkSize;
                break;
            }
            case HttpState::Done:
                return;
            }
        }
    }

    bool processBuffer()
    {
        switch (m_parseState) {
        case ParseState::Boundary: {
            const int pos = m_buffer.indexOf(kBoundary);
            if (pos < 0) {
                if (m_buffer.size() > 64) {
                    m_buffer.remove(0, m_buffer.size() - kBoundary.size());
                }
                return false;
            }
            const int end = pos + kBoundary.size();
            if (m_buffer.size() < end + 2) {
                return false;
            }
            if (m_buffer[end] != '\r' || m_buffer[end + 1] != '\n') {
                m_buffer.remove(pos, kBoundary.size());
                return true;
            }
            m_buffer.remove(0, end + 2);
            m_parseState = ParseState::Headers;
            return true;
        }
        case ParseState::Headers: {
            const int idx = m_buffer.indexOf("\r\n\r\n");
            if (idx < 0) {
                if (m_buffer.size() > 2048) {
                    fail(QStringLiteral("malformed part headers"));
                }
                return false;
            }
            const QByteArray headers = m_buffer.left(idx);
            m_contentLength = -1;
            const QList<QByteArray> lines = headers.split('\r');
            for (const QByteArray &line : lines) {
                const QByteArray trimmed = line.trimmed();
                const int colon = trimmed.indexOf(':');
                if (colon <= 0) {
                    continue;
                }
                const QByteArray key = trimmed.left(colon).trimmed().toLower();
                if (key == "content-length") {
                    m_contentLength = trimmed.mid(colon + 1).trimmed().toInt();
                }
            }
            if (m_contentLength <= 0) {
                fail(QStringLiteral("missing or invalid Content-Length"));
                return false;
            }
            m_buffer.remove(0, idx + 4);
            m_parseState = ParseState::Body;
            return true;
        }
        case ParseState::Body: {
            if (m_buffer.size() < m_contentLength) {
                return false;
            }
            const QByteArray payload = m_buffer.left(m_contentLength);
            m_buffer.remove(0, m_contentLength);
            m_parseState = ParseState::Boundary;

            QImage image;
            if (image.loadFromData(payload, "JPG") && !image.isNull()) {
                ++m_received;
                emit frameReady(image);
            } else {
                ++m_dropped;
            }
            return !m_buffer.isEmpty();
        }
        }
        return false;
    }

    QTcpSocket *m_socket = nullptr;
    HttpState m_httpState = HttpState::RespHeaders;
    ParseState m_parseState = ParseState::Boundary;
    QByteArray m_raw;
    QByteArray m_buffer;
    int m_contentLength = 0;
    int m_chunkRemaining = 0;
    bool m_chunked = false;
    bool m_active = false;
    quint32 m_received = 0;
    quint32 m_dropped = 0;
    qint64 m_bytes = 0;
    QString m_error;
};

MjpegClient::MjpegClient(QObject *parent)
    : QObject(parent)
    , m_thread(new QThread(this))
    , m_worker(new MjpegWorker)
{
    m_thread->setObjectName(QStringLiteral("mjpeg-net"));
    m_worker->moveToThread(m_thread);

    connect(m_worker, &MjpegWorker::activeUpdated, this, [this](bool active) {
        if (m_active != active) {
            m_active = active;
            emit activeChanged();
        }
    });
    connect(m_worker, &MjpegWorker::statsUpdated, this,
            [this](quint32 received, quint32 dropped, qint64 bytes) {
                if (m_framesReceived != received || m_framesDropped != dropped
                    || m_bytesReceived != bytes) {
                    m_framesReceived = received;
                    m_framesDropped = dropped;
                    m_bytesReceived = bytes;
                    emit statsChanged();
                }
            });
    connect(m_worker, &MjpegWorker::errorUpdated, this, [this](const QString &err) {
        if (m_errorString != err) {
            m_errorString = err;
            emit errorStringChanged();
        }
    });
    connect(m_worker, &MjpegWorker::frameReady, this, &MjpegClient::frameReady);

    m_thread->start();
}

MjpegClient::~MjpegClient()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    }
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
    m_worker = nullptr;
}

bool MjpegClient::isActive() const
{
    return m_active;
}

quint32 MjpegClient::framesReceived() const
{
    return m_framesReceived;
}

quint32 MjpegClient::framesDropped() const
{
    return m_framesDropped;
}

qint64 MjpegClient::bytesReceived() const
{
    return m_bytesReceived;
}

QString MjpegClient::errorString() const
{
    return m_errorString;
}

void MjpegClient::start(const QString &host, quint16 port)
{
    if (host.trimmed().isEmpty()) {
        m_errorString = QStringLiteral("host is empty");
        emit errorStringChanged();
        return;
    }
    m_errorString.clear();
    emit errorStringChanged();
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection,
                              Q_ARG(QString, host.trimmed()), Q_ARG(quint16, port));
}

void MjpegClient::stop()
{
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
}

#include "MjpegClient.moc"
