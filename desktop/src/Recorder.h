#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

class QFile;

// Master Prompt 21. Receives original JPEG bytes and stores them without
// re-encoding; the payload must be recoverable byte-for-byte.
//
// Layout of a .scamrec file:
//   char[8]  magic "SCAMREC\1"
//   u32le    header length
//   char[n]  JSON header (device, firmware, resolution, quality, started)
//   repeated:
//     u32le  frame length
//     char[] raw JPEG exactly as received
//
// The index lives in a sidecar .json and is a convenience, not a dependency:
// the container alone is sufficient to rebuild it, which is what makes the
// file recoverable after the application is killed mid-write.
class Recorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(QString path READ path NOTIFY recordingChanged)
    Q_PROPERTY(QString indexPath READ indexPath NOTIFY recordingChanged)
    Q_PROPERTY(qint64 framesWritten READ framesWritten NOTIFY progressChanged)
    Q_PROPERTY(qint64 bytesWritten READ bytesWritten NOTIFY progressChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    struct FrameEntry
    {
        qint64 offset = 0;
        int seq = 0;
        qint64 timestampMs = 0;
        qint64 bytes = 0;
        int width = 0;
        int height = 0;
    };

    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool isRecording() const;
    QString path() const;
    QString indexPath() const;
    qint64 framesWritten() const;
    qint64 bytesWritten() const;
    QString errorString() const;

    Q_INVOKABLE bool start(const QString &directory, const QJsonObject &meta);
    Q_INVOKABLE void stop();
    void appendFrame(const QByteArray &jpeg, int seq, qint64 timestampMs, int width,
                     int height);

    // Rebuilds the frame table from a container alone. Stops at the first
    // frame that is not a well-formed length-prefixed JPEG, which is exactly
    // where a truncated write ends - everything before it is intact.
    static bool scan(const QString &file, QVector<FrameEntry> *frames, QJsonObject *header,
                     QString *error);

    static QByteArray magic();

signals:
    void recordingChanged();
    void progressChanged();
    void errorStringChanged();

private:
    void setError(const QString &err);
    void writeIndex();
    void closeFile();

    QFile *m_file = nullptr;
    QString m_path;
    QString m_indexPath;
    QString m_error;
    QVector<FrameEntry> m_frames;
    qint64 m_bytesWritten = 0;
    QJsonObject m_meta;
    int m_framesSinceIndex = 0;
};
