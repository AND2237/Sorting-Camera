#include "MjpegClient.h"

#include "AppMetrics.h"

#include <QDateTime>
#include <QDebug>
#include <QObject>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace {
constexpr qint64 kMaxBufferBytes = 8 * 1024 * 1024;
const QByteArray kBoundary = "--FRAME";
constexpr int kMaxRetries = 5;
constexpr int kRetryDelayMs[kMaxRetries] = {500, 1000, 2000, 3000, 5000};
constexpr int kFirstByteTimeoutMs = 6000;
constexpr int kStallTimeoutMs = 8000;
constexpr int kWatchdogIntervalMs = 500;
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
            qDebug() << "[stream] replacing stale socket";
            QTcpSocket *stale = m_socket;
            m_socket = nullptr;
            stale->disconnect(this);
            stale->abort();
            stale->deleteLater();
        }
        resetParser();
        m_error.clear();
        emit errorUpdated(QString());
        qDebug() << "[stream] start" << host << port;

        m_socket = new QTcpSocket(this);
        const QString hostName = host;
        connect(m_socket, &QTcpSocket::connected, this, [this, hostName]() {
            qDebug() << "[stream] tcp connected";
            const QByteArray req = "GET /stream HTTP/1.1\r\nHost: " + hostName.toUtf8()
                                   + "\r\nConnection: close\r\n\r\n";
            m_socket->write(req);
        });
        connect(m_socket, &QTcpSocket::readyRead, this, &MjpegWorker::onReadyRead);
        connect(m_socket, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError e) {
                    qDebug() << "[stream] socket error" << e
                             << (m_socket ? m_socket->errorString() : QStringLiteral("no socket"));
                    fail(m_socket ? m_socket->errorString() : QStringLiteral("socket error"));
                });
        connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
            qDebug() << "[stream] disconnected active=" << m_active;
            if (m_active) {
                m_active = false;
                emit activeUpdated(false);
            }
            closeSocket();
        });

        m_socket->connectToHost(host, port);
        armWatchdog();
    }

    void stop()
    {
        stopWatchdog();
        if (m_socket) {
            qDebug() << "[stream] stop (local abort)";
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
    void frameReady(const QImage &image, qint64 completeMs);

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

    void armWatchdog()
    {
        if (!m_watchdog) {
            m_watchdog = new QTimer(this);
            m_watchdog->setInterval(kWatchdogIntervalMs);
            connect(m_watchdog, &QTimer::timeout, this, &MjpegWorker::checkLiveness);
        }
        m_hadData = false;
        m_lastDataMs = QDateTime::currentMSecsSinceEpoch();
        m_watchdog->start();
    }

    void stopWatchdog()
    {
        if (m_watchdog) {
            m_watchdog->stop();
        }
    }

    void checkLiveness()
    {
        if (!m_socket) {
            return;
        }
        const qint64 idleMs = QDateTime::currentMSecsSinceEpoch() - m_lastDataMs;
        if (!m_hadData) {
            if (idleMs > kFirstByteTimeoutMs) {
                fail(QStringLiteral("no response from camera %1 ms after connect")
                         .arg(kFirstByteTimeoutMs));
            }
        } else if (idleMs > kStallTimeoutMs) {
            fail(QStringLiteral("stream stalled: no data for %1 ms").arg(kStallTimeoutMs));
        }
    }

    void closeSocket()
    {
        stopWatchdog();
        if (!m_socket) {
            return;
        }
        m_socket->deleteLater();
        m_socket = nullptr;
        resetParser();
    }

    void fail(const QString &reason)
    {
        qDebug() << "[stream] fail:" << reason;
        stopWatchdog();
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
        const qint64 t_enter = AppMetrics::nowMs();
        if (m_lastReadMs > 0) {
            AppMetrics::instance().addReceiveGapMs(t_enter - m_lastReadMs);
        }
        m_lastReadMs = t_enter;

        const QByteArray chunk = m_socket->readAll();
        if (!chunk.isEmpty()) {
            m_hadData = true;
            m_lastDataMs = QDateTime::currentMSecsSinceEpoch();
        }
        AppMetrics::instance().countBytes(chunk.size());
        m_bytes += chunk.size();
        m_raw.append(chunk);

        if (m_buffer.size() + m_raw.size() > kMaxBufferBytes) {
            fail(QStringLiteral("stream buffer overflow (%1 bytes)")
                     .arg(m_buffer.size() + m_raw.size()));
            return;
        }

        const qint64 t_parse0 = AppMetrics::nowUs();
        pump();
        AppMetrics::instance().addParseUs(AppMetrics::nowUs() - t_parse0);

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
            const qint64 completeMs = AppMetrics::nowMs();
            m_frameCompleteMs = completeMs;
            AppMetrics::instance().countPart();

            const qint64 t_decode0 = AppMetrics::nowUs();
            QImage image;
            const bool ok = image.loadFromData(payload, "JPG") && !image.isNull();
            AppMetrics::instance().addDecodeUs(AppMetrics::nowUs() - t_decode0);
            if (ok) {
                ++m_received;
                AppMetrics::instance().countDecoded();
                if (!m_active) {
                    m_active = true;
                    emit activeUpdated(true);
                }
                emit frameReady(image, completeMs);
            } else {
                ++m_dropped;
                AppMetrics::instance().countDecodeFailed();
            }
            return !m_buffer.isEmpty();
        }
        }
        return false;
    }

    QTcpSocket *m_socket = nullptr;
    QTimer *m_watchdog = nullptr;
    qint64 m_lastDataMs = 0;
    qint64 m_lastReadMs = 0;
    qint64 m_frameCompleteMs = 0;
    bool m_hadData = false;
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

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_userConnected) {
            invokeStart();
        }
    });

    connect(m_worker, &MjpegWorker::activeUpdated, this, [this](bool active) {
        if (m_active != active) {
            m_active = active;
            emit activeChanged();
        }
        if (active) {
            setConnecting(false);
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
        if (err.isEmpty()) {
            setError(QString());
            return;
        }
        const bool noResponse = err.startsWith(QStringLiteral("no response from camera"));
        if (noResponse) {
            setNoResponseStreak(m_noResponseStreak + 1);
            if (m_noResponseStreak == 2 && m_recoveriesTriggered < 2) {
                m_recoveriesTriggered++;
                setRecoveryHint(QStringLiteral("camera not responding, asking device to re-init…"));
                emit deviceRecoveryRequested(m_host, m_port);
            }
        }
        if (m_userConnected && m_retryAttempt < kMaxRetries) {
            const int attempt = m_retryAttempt + 1;
            setRetryAttempt(attempt);
            setConnecting(true);
            setReconnecting(true);
            setError(QString());
            qDebug() << "[stream] retry" << attempt << "/" << kMaxRetries << "in"
                     << kRetryDelayMs[attempt - 1] << "ms after:" << err;
            m_retryTimer->start(kRetryDelayMs[attempt - 1]);
            return;
        }
        setConnecting(false);
        setReconnecting(false);
        if (noResponse && m_recoveryHint.isEmpty()) {
            setError(QStringLiteral("%1. The camera may be wedged - use Recover camera.")
                         .arg(err));
            return;
        }
        setError(err);
    });
    connect(m_worker, &MjpegWorker::frameReady, this,
            [this](const QImage &image, qint64 completeMs) {
                AppMetrics::instance().addPresentAgeMs(AppMetrics::nowMs() - completeMs);
                setRetryAttempt(0);
                setConnecting(false);
                setNoResponseStreak(0);
                setReconnecting(false);
                setRecoveryHint(QString());
                emit frameReady(image, completeMs);
            });

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

bool MjpegClient::isReconnecting() const
{
    return m_reconnecting;
}

bool MjpegClient::isConnecting() const
{
    return m_connecting;
}

int MjpegClient::retryAttempt() const
{
    return m_retryAttempt;
}

int MjpegClient::noResponseStreak() const
{
    return m_noResponseStreak;
}

QString MjpegClient::recoveryHint() const
{
    return m_recoveryHint;
}

void MjpegClient::setError(const QString &err)
{
    if (m_errorString != err) {
        m_errorString = err;
        emit errorStringChanged();
    }
}

void MjpegClient::setReconnecting(bool reconnecting)
{
    if (m_reconnecting != reconnecting) {
        m_reconnecting = reconnecting;
        emit reconnectingChanged();
    }
}

void MjpegClient::setConnecting(bool connecting)
{
    if (m_connecting != connecting) {
        m_connecting = connecting;
        emit connectingChanged();
    }
}

void MjpegClient::setNoResponseStreak(int streak)
{
    if (m_noResponseStreak != streak) {
        m_noResponseStreak = streak;
        emit noResponseStreakChanged();
    }
}

void MjpegClient::setRecoveryHint(const QString &hint)
{
    if (m_recoveryHint != hint) {
        m_recoveryHint = hint;
        emit recoveryHintChanged();
    }
}

void MjpegClient::setRetryAttempt(int attempt)
{
    if (m_retryAttempt != attempt) {
        m_retryAttempt = attempt;
        emit retryAttemptChanged();
    }
}

void MjpegClient::cancelRetry()
{
    if (m_retryTimer && m_retryTimer->isActive()) {
        m_retryTimer->stop();
    }
}

void MjpegClient::invokeStart()
{
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection,
                              Q_ARG(QString, m_host), Q_ARG(quint16, m_port));
}

void MjpegClient::start(const QString &host, quint16 port)
{
    if (host.trimmed().isEmpty()) {
        setError(QStringLiteral("host is empty"));
        return;
    }
    m_host = host.trimmed();
    m_port = port;
    m_userConnected = true;
    cancelRetry();
    setRetryAttempt(0);
    setReconnecting(false);
    setConnecting(true);
    setNoResponseStreak(0);
    setRecoveryHint(QString());
    setError(QString());
    invokeStart();
}

void MjpegClient::stop()
{
    m_userConnected = false;
    cancelRetry();
    setReconnecting(false);
    setConnecting(false);
    setRetryAttempt(0);
    setNoResponseStreak(0);
    setRecoveryHint(QString());
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
}

#include "MjpegClient.moc"
