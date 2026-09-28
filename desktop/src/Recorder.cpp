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
    closeFile();
}

bool Recorder::isRecording() const
{
    return m_file && m_file->isOpen();
}

QString Recorder::path() const
{
    return m_path;
}

QString Recorder::indexPath() const
{
    return m_indexPath;
}

qint64 Recorder::framesWritten() const
{
    return m_frames.size();
}

qint64 Recorder::bytesWritten() const
{
    return m_bytesWritten;
}

QString Recorder::errorString() const
{
    return m_error;
}

void Recorder::setError(const QString &err)
{
    if (m_error != err) {
        m_error = err;
        emit errorStringChanged();
    }
}

bool Recorder::start(const QString &directory, const QJsonObject &meta)
{
    if (isRecording()) {
        setError(QStringLiteral("already recording"));
        return false;
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        setError(QStringLiteral("cannot create directory %1").arg(directory));
        return false;
    }

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    const QString base = dir.filePath(QStringLiteral("rec_%1.scamrec").arg(stamp));

    QFile *file = new QFile(base, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        setError(QStringLiteral("cannot open %1: %2").arg(base, file->errorString()));
        delete file;
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
        setError(QStringLiteral("cannot write header to %1: %2").arg(base, file->errorString()));
        delete file;
        return false;
    }

    m_file = file;
    m_path = base;
    m_indexPath = base + QStringLiteral(".json");
    m_meta = header;
    m_frames.clear();
    m_framesSinceIndex = 0;
    m_bytesWritten = head.size();
    setError(QString());
    emit recordingChanged();
    emit progressChanged();
    return true;
}

void Recorder::appendFrame(const QByteArray &jpeg, int seq, qint64 timestampMs, int width,
                           int height)
{
    if (!isRecording() || jpeg.isEmpty()) {
        return;
    }

    const quint32 len = quint32(jpeg.size());
    char lenBytes[4] = {char(len & 0xFF), char((len >> 8) & 0xFF), char((len >> 16) & 0xFF),
                        char((len >> 24) & 0xFF)};
    if (m_file->write(lenBytes, 4) != 4 || m_file->write(jpeg) != jpeg.size()) {
        setError(QStringLiteral("write failed: %1").arg(m_file->errorString()));
        closeFile();
        return;
    }

    FrameEntry e;
    e.offset = m_bytesWritten;
    e.seq = seq;
    e.timestampMs = timestampMs;
    e.bytes = qint64(len);
    e.width = width;
    e.height = height;
    m_frames.append(e);
    m_bytesWritten += 4 + jpeg.size();

    emit progressChanged();

    if (++m_framesSinceIndex >= kIndexEveryFrames) {
        m_framesSinceIndex = 0;
        writeIndex();
    }
}

void Recorder::writeIndex()
{
    if (m_indexPath.isEmpty()) {
        return;
    }

    QJsonObject root = m_meta;
    QJsonArray frames;
    for (const FrameEntry &e : m_frames) {
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
    root[QStringLiteral("file")] = QFileInfo(m_path).fileName();

    QSaveFile out(m_indexPath);
    if (!out.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("cannot write index: %1").arg(out.errorString()));
        return;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit()) {
        setError(QStringLiteral("cannot commit index: %1").arg(out.errorString()));
    }
}

void Recorder::stop()
{
    if (!isRecording()) {
        return;
    }
    writeIndex();
    closeFile();
    emit recordingChanged();
}

void Recorder::closeFile()
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
