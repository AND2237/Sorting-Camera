#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
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
//
// Threading: the frame path runs on the net thread (architecture.md:124-128
// puts both FrameBus and Recorder there) while start()/stop() and the QML
// property reads run on the GUI thread, so every piece of state below is
// guarded. Two locks with one fixed nesting: writeIndex() takes m_indexMutex
// and then m_mutex, and no other code takes them in the reverse order, so
// they cannot deadlock. Signals are always emitted after both are released,
// because a slot is free to read a property back and would otherwise
// re-enter the lock it is being emitted under.
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
    // Assigns m_error without emitting: every caller emits errorStringChanged()
    // itself, after releasing m_mutex, because the slot is free to read the
    // property back and would otherwise re-enter the lock it is under.
    void setErrorLocked(const QString &err);

    // Snapshots the frame table and its file paths, then serialises the
    // sidecar. Must be called with m_mutex NOT held: it takes m_indexMutex
    // first and m_mutex second, and that order is what guarantees the last
    // sidecar written is also the most complete. The expensive part runs
    // without m_mutex held, so the frame path is never blocked behind disk
    // I/O. Returns false and fills *err when the sidecar could not be
    // written; the caller records that after both locks are released.
    bool writeIndex(QString *err);
    void closeFileLocked();

    mutable QMutex m_mutex;
    // Serialises the sidecar write itself, which is the only part that runs
    // without m_mutex held. Taken before m_mutex inside writeIndex() and
    // never the other way round, so the two cannot deadlock.
    mutable QMutex m_indexMutex;

    QFile *m_file = nullptr;
    QString m_path;
    QString m_indexPath;
    QString m_error;
    QVector<FrameEntry> m_frames;
    qint64 m_bytesWritten = 0;
    QJsonObject m_meta;
    int m_framesSinceIndex = 0;
};
