#include "Recorder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>

namespace {
constexpr int kIndexEveryFrames = 60;

bool looksLikeJpeg(const QByteArray &jpeg)
{
    if (jpeg.size() < 4) {
        return false;
    }
    const auto *b = reinterpret_cast<const uchar *>(jpeg.constData());
    return b[0] == 0xFF && b[1] == 0xD8 && b[jpeg.size() - 2] == 0xFF
           && b[jpeg.size() - 1] == 0xD9;
}

quint32 readU32(QIODevice *dev, bool *ok)
{
    char buf[4];
    *ok = dev->read(buf, 4) == 4;
    if (!*ok) {
        return 0;
    }
    return quint32(uchar(buf[0])) | (quint32(uchar(buf[1])) << 8)
           | (quint32(uchar(buf[2])) << 16) | (quint32(uchar(buf[3])) << 24);
}
} // namespace

QByteArray Recorder::magic()
{
    return QByteArray("SCAMREC\1", 8);
}

Recorder::Recorder(QObject *parent) : QObject(parent) {}

Recorder::~Recorder()
{
    QMutexLocker lock(&m_mutex);
    closeFileLocked();
}

bool Recorder::isRecording() const
{
    QMutexLocker lock(&m_mutex);
    return m_file && m_file->isOpen();
}

QString Recorder::path() const
{
    QMutexLocker lock(&m_mutex);
    return m_path;
}

QString Recorder::indexPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_indexPath;
}

qint64 Recorder::framesWritten() const
{
    QMutexLocker lock(&m_mutex);
    return m_frames.size();
}

qint64 Recorder::bytesWritten() const
{
    QMutexLocker lock(&m_mutex);
    return m_bytesWritten;
}

QString Recorder::errorString() const
{
    QMutexLocker lock(&m_mutex);
    return m_error;
}

void Recorder::setErrorLocked(const QString &err)
{
    m_error = err;
}

bool Recorder::start(const QString &directory, const QJsonObject &meta)
{
    QString err;
    {
        QMutexLocker lock(&m_mutex);
        if (m_file && m_file->isOpen()) {
            setErrorLocked(QStringLiteral("already recording"));
            err = m_error;
        }
    }
    if (!err.isEmpty()) {
        emit errorStringChanged();
        return false;
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        const QString msg = QStringLiteral("cannot create directory %1").arg(directory);
        {
            QMutexLocker lock(&m_mutex);
            setErrorLocked(msg);
        }
        emit errorStringChanged();
        return false;
    }

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    const QString base = dir.filePath(QStringLiteral("rec_%1.scamrec").arg(stamp));

    QFile *file = new QFile(base, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        const QString msg =
            QStringLiteral("cannot open %1: %2").arg(base, file->errorString());
        delete file;
        {
            QMutexLocker lock(&m_mutex);
            setErrorLocked(msg);
        }
        emit errorStringChanged();
        return false;
    }

    QJsonObject header = meta;
    header[QStringLiteral("format")] = QStringLiteral("scamrec");
    header[QStringLiteral("formatVersion")] = 1;
    header[QStringLiteral("started")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    const QByteArray headerBytes = QJsonDocument(header).toJson(QJsonDocument::Compact);

    QByteArray head;
    head.append(magic());
    const quint32 headerLen = quint32(headerBytes.size());
    head.append(char(headerLen & 0xFF));
    head.append(char((headerLen >> 8) & 0xFF));
    head.append(char((headerLen >> 16) & 0xFF));
    head.append(char((headerLen >> 24) & 0xFF));
    head.append(headerBytes);

    if (file->write(head) != head.size()) {
        const QString msg = QStringLiteral("cannot write header to %1: %2")
                                .arg(base, file->errorString());
        delete file;
        {
            QMutexLocker lock(&m_mutex);
            setErrorLocked(msg);
        }
        emit errorStringChanged();
        return false;
    }

    {
        QMutexLocker lock(&m_mutex);
        m_file = file;
        m_path = base;
        m_indexPath = base + QStringLiteral(".json");
        m_meta = header;
        m_frames.clear();
        m_framesSinceIndex = 0;
        m_bytesWritten = head.size();
        m_error.clear();
    }
    emit recordingChanged();
    emit progressChanged();
    return true;
}

void Recorder::appendFrame(const QByteArray &jpeg, int seq, qint64 timestampMs, int width,
                           int height)
{
    if (jpeg.isEmpty()) {
        return;
    }

    bool flushIndex = false;
    bool errorChanged = false;
    bool didRecord = false;
    bool didProgress = false;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_file || !m_file->isOpen()) {
            return;
        }

        const quint32 len = quint32(jpeg.size());
        char lenBytes[4] = {char(len & 0xFF), char((len >> 8) & 0xFF),
                            char((len >> 16) & 0xFF), char((len >> 24) & 0xFF)};
        if (m_file->write(lenBytes, 4) != 4 || m_file->write(jpeg) != jpeg.size()) {
            setErrorLocked(QStringLiteral("write failed: %1").arg(m_file->errorString()));
            closeFileLocked();
            errorChanged = true;
            didRecord = true;
        } else {
            FrameEntry e;
            e.offset = m_bytesWritten;
            e.seq = seq;
            e.timestampMs = timestampMs;
            e.bytes = qint64(len);
            e.width = width;
            e.height = height;
            m_frames.append(e);
            m_bytesWritten += 4 + jpeg.size();
            didProgress = true;

            if (++m_framesSinceIndex >= kIndexEveryFrames) {
                m_framesSinceIndex = 0;
                flushIndex = true;
            }
        }
    }

    // Outside the lock: a sidecar flush is the one part of recording that does
    // real work proportional to the length of the take, and the frame path
    // must not wait behind it.
    if (flushIndex) {
        QString err;
        if (!writeIndex(&err)) {
            QMutexLocker lock(&m_mutex);
            setErrorLocked(err);
            errorChanged = true;
        }
    }

    if (errorChanged) {
        emit errorStringChanged();
    }
    if (didRecord) {
        emit recordingChanged();
    }
    if (didProgress) {
        emit progressChanged();
    }
}

bool Recorder::writeIndex(QString *err)
{
    // m_indexMutex is the outer lock and m_mutex the inner one here - the
    // reverse of nowhere: nothing ever takes m_mutex first and m_indexMutex
    // after, so the pair cannot deadlock. Taking them in this order is what
    // makes the final sidecar agree with the container. stop() has already
    // closed the file by the time it calls this, so the snapshot below is
    // taken after the last append either way round two flushes interleave:
    // the one that acquires m_indexMutex later always sees the closed file
    // and therefore the complete frame table.
    QMutexLocker indexLock(&m_indexMutex);

    QVector<FrameEntry> snapshot;
    QString indexPath;
    QString filePath;
    QJsonObject meta;
    {
        QMutexLocker lock(&m_mutex);
        if (m_indexPath.isEmpty()) {
            return true;
        }
        // Implicitly shared, so this is a reference count and not a copy: the
        // frame path keeps appending while the index is serialised. The next
        // append detaches once, an O(N) memcpy that replaces the O(N) JSON
        // build this used to hold the frame lock through.
        snapshot = m_frames;
        indexPath = m_indexPath;
        filePath = m_path;
        meta = m_meta;
    }

    QJsonObject root = meta;
    QJsonArray frames;
    for (const FrameEntry &e : snapshot) {
        QJsonObject f;
        f[QStringLiteral("offset")] = double(e.offset);
        f[QStringLiteral("seq")] = e.seq;
        f[QStringLiteral("ts")] = double(e.timestampMs);
        f[QStringLiteral("bytes")] = double(e.bytes);
        f[QStringLiteral("w")] = e.width;
        f[QStringLiteral("h")] = e.height;
        frames.append(f);
    }
    root[QStringLiteral("frames")] = frames;
    root[QStringLiteral("file")] = QFileInfo(filePath).fileName();

    QSaveFile out(indexPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (err) {
            *err = QStringLiteral("cannot write index: %1").arg(out.errorString());
        }
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit()) {
        if (err) {
            *err = QStringLiteral("cannot commit index: %1").arg(out.errorString());
        }
        return false;
    }
    return true;
}

void Recorder::stop()
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_file || !m_file->isOpen()) {
            return;
        }
        // Close first so no frame can be appended after the snapshot the index
        // is built from. appendFrame() takes the same lock, so a frame already
        // in flight either lands before the close and is indexed, or sees no
        // file and returns.
        closeFileLocked();
    }

    QString err;
    const bool indexed = writeIndex(&err);
    if (!indexed) {
        QMutexLocker lock(&m_mutex);
        setErrorLocked(err);
    }

    emit recordingChanged();
    emit progressChanged();
    if (!indexed) {
        emit errorStringChanged();
    }
}

void Recorder::closeFileLocked()
{
    if (!m_file) {
        return;
    }
    m_file->flush();
    m_file->close();
    delete m_file;
    m_file = nullptr;
}

bool Recorder::scan(const QString &file, QVector<FrameEntry> *frames, QJsonObject *header,
                    QString *error)
{
    if (frames) {
        frames->clear();
    }
    if (header) {
        *header = QJsonObject();
    }

    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open %1: %2").arg(file, in.errorString());
        }
        return false;
    }

    const QByteArray head = in.read(12);
    if (head.size() < 12 || !head.startsWith(magic())) {
        if (error) {
            *error = QStringLiteral("%1 is not a scamrec container").arg(file);
        }
        return false;
    }
    const auto *ub = reinterpret_cast<const uchar *>(head.constData() + 8);
    const quint32 headerLen = quint32(ub[0]) | (quint32(ub[1]) << 8) | (quint32(ub[2]) << 16)
                              | (quint32(ub[3]) << 24);
    if (headerLen > 1u << 20) {
        if (error) {
            *error = QStringLiteral("%1 has an implausible header").arg(file);
        }
        return false;
    }
    const QByteArray headerBytes = in.read(headerLen);
    if (quint32(headerBytes.size()) != headerLen) {
        if (error) {
            *error = QStringLiteral("%1 has a truncated header").arg(file);
        }
        return false;
    }
    if (header) {
        *header = QJsonDocument::fromJson(headerBytes).object();
    }

    qint64 offset = 12 + headerLen;
    int seq = 0;
    for (;;) {
        bool ok = false;
        const quint32 len = readU32(&in, &ok);
        if (!ok) {
            break; // clean end of file
        }
        if (len == 0 || len > (1u << 24)) {
            break; // garbage length: the tail was cut short
        }
        const QByteArray jpeg = in.read(len);
        if (quint32(jpeg.size()) != len) {
            break; // truncated frame: everything before this point is intact
        }
        if (!looksLikeJpeg(jpeg)) {
            break;
        }
        if (frames) {
            FrameEntry e;
            e.offset = offset;
            e.seq = seq;
            e.bytes = len;
            frames->append(e);
        }
        ++seq;
        offset += 4 + len;
    }
    return true;
}
