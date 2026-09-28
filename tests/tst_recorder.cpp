#include "FrameBus.h"
#include "Recorder.h"
#include "SnapshotWriter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Enough like a JPEG to pass the container's marker check, and with a body
// that would be caught by any byte-level comparison.
QByteArray fakeJpeg(int size, quint8 seed)
{
    QByteArray b;
    b.reserve(size);
    b.append(char(0xFF));
    b.append(char(0xD8));
    for (int i = 2; i < size - 2; i++) {
        b.append(char((seed + i * 7) & 0xFF));
    }
    b.append(char(0xFF));
    b.append(char(0xD9));
    return b;
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

} // namespace

class TestRecorder : public QObject
{
    Q_OBJECT

private slots:
    void roundTripIsByteExact();
    void scanRebuildsTheIndex();
    void headerSurvivesRoundTrip();
    void truncatedTailKeepsIntactFrames();
    void refusingToStartTwice();
    void snapshotIsByteExact();
    void snapshotRefusesWithoutAFrame();
};

void TestRecorder::roundTripIsByteExact()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QList<QByteArray> payloads;
    for (int i = 0; i < 12; i++) {
        payloads.append(fakeJpeg(64 + i * 37, quint8(i)));
    }

    QString path;
    {
        Recorder rec;
        QJsonObject meta;
        meta[QStringLiteral("device_id")] = QStringLiteral("b4bfe9343ae0");
        QVERIFY2(rec.start(dir.path(), meta), qPrintable(rec.errorString()));
        QVERIFY(rec.isRecording());
        for (int i = 0; i < payloads.size(); i++) {
            rec.appendFrame(payloads.at(i), i, 1000 + i, 1280, 720);
        }
        QCOMPARE(rec.framesWritten(), qint64(payloads.size()));
        rec.stop();
        QVERIFY(!rec.isRecording());
        path = rec.path();
    }

    QVector<Recorder::FrameEntry> entries;
    QJsonObject header;
    QString err;
    QVERIFY2(Recorder::scan(path, &entries, &header, &err), qPrintable(err));
    QCOMPARE(entries.size(), payloads.size());

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    for (int i = 0; i < entries.size(); i++) {
        const Recorder::FrameEntry &e = entries.at(i);
        QCOMPARE(e.seq, i);
        QVERIFY(f.seek(e.offset + 4));
        const QByteArray got = f.read(e.bytes);
        QCOMPARE(sha256(got), sha256(payloads.at(i)));
        QCOMPARE(got, payloads.at(i));
    }
}

void TestRecorder::scanRebuildsTheIndex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Recorder rec;
    QJsonObject meta;
    QVERIFY(rec.start(dir.path(), meta));
    for (int i = 0; i < 70; i++) {
        rec.appendFrame(fakeJpeg(120, quint8(i)), i, 5000 + i, 640, 480);
    }
    const QString indexPath = rec.indexPath();
    rec.stop();

    // 70 frames crosses the periodic-index threshold, so the sidecar exists
    // without stop() having written it.
    QVERIFY(QFile::exists(indexPath));
    const QJsonObject sidecar =
        QJsonDocument::fromJson([&]() {
            QFile s(indexPath);
            s.open(QIODevice::ReadOnly);
            return s.readAll();
        }())
            .object();
    const QJsonArray frames = sidecar.value(QStringLiteral("frames")).toArray();
    QCOMPARE(frames.size(), 70);
    QCOMPARE(frames.at(69).toObject().value(QStringLiteral("w")).toInt(), 640);
    QCOMPARE(frames.at(69).toObject().value(QStringLiteral("h")).toInt(), 480);
    QCOMPARE(frames.at(69).toObject().value(QStringLiteral("ts")).toDouble(), 5069.0);

    QVector<Recorder::FrameEntry> entries;
    QString err;
    QVERIFY2(Recorder::scan(rec.path(), &entries, nullptr, &err), qPrintable(err));
    QCOMPARE(entries.size(), 70);
    QCOMPARE(entries.at(69).bytes, qint64(120));
}

void TestRecorder::headerSurvivesRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Recorder rec;
    QJsonObject meta;
    meta[QStringLiteral("device_id")] = QStringLiteral("b4bfe9343ae0");
    meta[QStringLiteral("firmware")] = QStringLiteral("v0.1.0");
    meta[QStringLiteral("framesize")] = QStringLiteral("hd");
    QVERIFY(rec.start(dir.path(), meta));
    rec.appendFrame(fakeJpeg(80, 3), 0, 1, 1280, 720);
    rec.stop();

    QJsonObject header;
    QString err;
    QVERIFY2(Recorder::scan(rec.path(), nullptr, &header, &err), qPrintable(err));
    QCOMPARE(header.value(QStringLiteral("device_id")).toString(),
             QStringLiteral("b4bfe9343ae0"));
    QCOMPARE(header.value(QStringLiteral("firmware")).toString(), QStringLiteral("v0.1.0"));
    QCOMPARE(header.value(QStringLiteral("formatVersion")).toInt(), 1);
    QVERIFY(!header.value(QStringLiteral("started")).toString().isEmpty());
}

void TestRecorder::truncatedTailKeepsIntactFrames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QList<QByteArray> payloads = {fakeJpeg(100, 1), fakeJpeg(110, 2),
                                        fakeJpeg(120, 3), fakeJpeg(130, 4)};
    QString path;
    {
        Recorder rec;
        QJsonObject meta;
        QVERIFY(rec.start(dir.path(), meta));
        for (int i = 0; i < payloads.size(); i++) {
            rec.appendFrame(payloads.at(i), i, i, 640, 480);
        }
        // Deliberately no stop(): the application was killed here, so the
        // sidecar was never written for these frames.
        path = rec.path();
    }

    QVERIFY(QFile::exists(path));
    QVERIFY(!QFile::exists(path + QStringLiteral(".json")));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.resize(f.size() - 10));
    f.close();

    QVector<Recorder::FrameEntry> entries;
    QString err;
    QVERIFY2(Recorder::scan(path, &entries, nullptr, &err), qPrintable(err));
    QCOMPARE(entries.size(), payloads.size() - 1);

    QFile r(path);
    QVERIFY(r.open(QIODevice::ReadOnly));
    for (int i = 0; i < entries.size(); i++) {
        QVERIFY(r.seek(entries.at(i).offset + 4));
        QCOMPARE(r.read(entries.at(i).bytes), payloads.at(i));
    }
}

void TestRecorder::refusingToStartTwice()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Recorder rec;
    QJsonObject meta;
    QVERIFY(rec.start(dir.path(), meta));
    QVERIFY(!rec.start(dir.path(), meta));
    rec.stop();
}

void TestRecorder::snapshotIsByteExact()
{
    // Build a real JPEG so FrameBus accepts it, then keep those bytes as the
    // "network payload" and prove the file on disk is identical to them.
    QImage img(32, 24, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray payload;
    {
        QBuffer buf(&payload);
        QVERIFY(buf.open(QIODevice::WriteOnly));
        QVERIFY(img.save(&buf, "JPG", 7));
    }
    QVERIFY(payload.startsWith(QByteArrayLiteral("\xFF\xD8")));
    const QByteArray expectedHash = sha256(payload);

    FrameBus bus;
    bus.setFrame(img, payload, 1234);
    QVERIFY(bus.hasRawFrame());

    SnapshotWriter writer(&bus);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = writer.save(dir.path(), QStringLiteral("b4bfe9343ae0"));
    QVERIFY2(!path.isEmpty(), qPrintable(writer.errorString()));
    QVERIFY(path.endsWith(QStringLiteral(".jpg")));

    QFile out(path);
    QVERIFY(out.open(QIODevice::ReadOnly));
    const QByteArray written = out.readAll();
    QCOMPARE(sha256(written), expectedHash);
    QCOMPARE(written.size(), payload.size());
    QVERIFY(QFileInfo(path).size() == payload.size());
    QCOMPARE(writer.lastPath(), path);
}

void TestRecorder::snapshotRefusesWithoutAFrame()
{
    FrameBus bus;
    QVERIFY(!bus.hasRawFrame());

    SnapshotWriter writer(&bus);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = writer.save(dir.path(), QString());
    QVERIFY(path.isEmpty());
    QVERIFY(!writer.errorString().isEmpty());
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("snap"))));
}

QTEST_GUILESS_MAIN(TestRecorder)
#include "tst_recorder.moc"
