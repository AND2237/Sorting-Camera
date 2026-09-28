#include "FrameBus.h"
#include "MjpegClient.h"
#include "Recorder.h"
#include "SnapshotWriter.h"
#include "StreamStats.h"

#include <QBuffer>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace {

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// Serves a short multipart/x-mixed-replace response in exactly the shape the
// firmware does, so the real parser, the real worker thread and the real
// byte-preservation path are all exercised.
class MjpegStub : public QObject
{
    Q_OBJECT
public:
    explicit MjpegStub(const QList<QByteArray> &payloads, QObject *parent = nullptr)
        : QObject(parent), m_payloads(payloads)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MjpegStub::onConnection);
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return m_server.serverPort(); }

    // Pushes more parts onto an already-open connection, so a test can create
    // traffic inside a measurement window rather than only before it.
    void sendMore(const QList<QByteArray> &payloads)
    {
        if (!m_socket) {
            return;
        }
        m_socket->write(encode(payloads));
        m_socket->flush();
    }

private:
    static QByteArray encode(const QList<QByteArray> &payloads)
    {
        QByteArray out;
        for (const QByteArray &p : payloads) {
            out += "--FRAME\r\n";
            out += "Content-Length: " + QByteArray::number(p.size()) + "\r\n\r\n";
            out += p;
        }
        return out;
    }

    void onConnection()
    {
        QTcpSocket *sock = m_server.nextPendingConnection();
        m_socket = sock;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            m_request.append(sock->readAll());
            if (m_sent || !m_request.contains("\r\n\r\n")) {
                return;
            }
            m_sent = true;
            QByteArray out;
            out += "HTTP/1.1 200 OK\r\n";
            out += "Content-Type: multipart/x-mixed-replace; boundary=--FRAME\r\n";
            out += "Cache-Control: no-cache\r\n\r\n";
            out += encode(m_payloads);
            sock->write(out);
            sock->flush();
        });
    }

    QTcpServer m_server;
    QTcpSocket *m_socket = nullptr;
    QList<QByteArray> m_payloads;
    QByteArray m_request;
    bool m_sent = false;
};

QList<QByteArray> realJpegs(int count)
{
    QList<QByteArray> payloads;
    for (int i = 0; i < count; i++) {
        QImage img(48, 32, QImage::Format_RGB32);
        img.fill(QColor((i * 37) % 256, (i * 91) % 256, (i * 13) % 256));
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "JPG", 5 + i)) {
            return {};
        }
        payloads.append(bytes);
    }
    return payloads;
}

} // namespace

class TestCapture : public QObject
{
    Q_OBJECT

private slots:
    void rawBytesSurviveTheStream();
    void snapshotMatchesWhatTheCameraSent();
    void recordingRoundTripsTheStreamedBytes();
    void bitrateAndFrameAgeReportLiveValues();
    void unreadFramesAreCountedAsOverwritten();
    void readFramesAreNotCountedAsOverwritten();
};

void TestCapture::rawBytesSurviveTheStream()
{
    const QList<QByteArray> payloads = realJpegs(4);
    QVERIFY(payloads.size() == 4);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());

    FrameBus bus;
    MjpegClient stream;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
            });

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(bus.version() >= payloads.size(), 8000);
    stream.stop();

    QCOMPARE(bus.rawFrame(), payloads.last());
    QCOMPARE(sha256(bus.rawFrame()), sha256(payloads.last()));
    QVERIFY(bus.hasRawFrame());
}

void TestCapture::snapshotMatchesWhatTheCameraSent()
{
    const QList<QByteArray> payloads = realJpegs(3);
    QVERIFY(payloads.size() == 3);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());

    FrameBus bus;
    MjpegClient stream;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
            });

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(bus.version() >= payloads.size(), 8000);

    SnapshotWriter writer(&bus);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writer.save(dir.path(), QStringLiteral("testdev"));
    QVERIFY2(!path.isEmpty(), qPrintable(writer.errorString()));
    stream.stop();

    QFile out(path);
    QVERIFY(out.open(QIODevice::ReadOnly));
    const QByteArray written = out.readAll();
    QCOMPARE(sha256(written), sha256(payloads.last()));
    QCOMPARE(written, payloads.last());
}

void TestCapture::recordingRoundTripsTheStreamedBytes()
{
    const QList<QByteArray> payloads = realJpegs(5);
    QVERIFY(payloads.size() == 5);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());

    FrameBus bus;
    MjpegClient stream;
    Recorder recorder;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QJsonObject meta;
    meta[QStringLiteral("device_id")] = QStringLiteral("stub");
    QString container;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus, &recorder](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
                if (recorder.isRecording() && !raw.isEmpty()) {
                    recorder.appendFrame(raw, int(recorder.framesWritten()),
                                         QDateTime::currentMSecsSinceEpoch(),
                                         img.width(), img.height());
                }
            });

    QVERIFY2(recorder.start(dir.path(), meta), qPrintable(recorder.errorString()));
    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(recorder.framesWritten() >= payloads.size(), 8000);
    recorder.stop();
    stream.stop();
    container = recorder.path();

    QVector<Recorder::FrameEntry> entries;
    QString err;
    QVERIFY2(Recorder::scan(container, &entries, nullptr, &err), qPrintable(err));
    QCOMPARE(entries.size(), payloads.size());

    QFile f(container);
    QVERIFY(f.open(QIODevice::ReadOnly));
    for (int i = 0; i < entries.size(); i++) {
        QVERIFY(f.seek(entries.at(i).offset + 4));
        const QByteArray got = f.read(entries.at(i).bytes);
        QCOMPARE(sha256(got), sha256(payloads.at(i)));
    }
}

void TestCapture::unreadFramesAreCountedAsOverwritten()
{
    FrameBus bus;
    const QImage img(64, 48, QImage::Format_RGB32);
    const QByteArray raw = realJpegs(1).value(0);

    QCOMPARE(bus.overwrittenCount(), 0);
    bus.setFrame(img, raw, 1000);
    QCOMPARE(bus.overwrittenCount(), 0);

    // A frame nobody has read when the next one arrives is a frame the user
    // never saw. Latest-frame-wins is the correct policy for a live view, but
    // the replacement still has to be counted - AGENTS.md is explicit that
    // every drop is counted and never hidden.
    bus.setFrame(img, raw, 1042);
    QCOMPARE(bus.overwrittenCount(), 1);
    bus.setFrame(img, raw, 1084);
    QCOMPARE(bus.overwrittenCount(), 2);

    // The newest frame is still what is displayed, and the raw bytes are
    // untouched by all of this.
    QCOMPARE(bus.version(), 3);
    QCOMPARE(bus.rawFrame(), raw);
}

void TestCapture::readFramesAreNotCountedAsOverwritten()
{
    FrameBus bus;
    const QImage img(64, 48, QImage::Format_RGB32);
    const QByteArray raw = realJpegs(1).value(0);

    for (int i = 0; i < 5; ++i) {
        bus.setFrame(img, raw, 1000 + i * 42);
        // Reading the frame is what marks it as seen. Without this the counter
        // would report a healthy 5 fps stream as five dropped frames.
        QVERIFY(!bus.image().isNull());
    }
    // Every frame was displayed, so nothing was overwritten - a counter that
    // fired here would train the reader to ignore it.
    QCOMPARE(bus.overwrittenCount(), 0);
}

void TestCapture::bitrateAndFrameAgeReportLiveValues()
{
    const QList<QByteArray> first = realJpegs(2);
    const QList<QByteArray> second = realJpegs(2);
    QVERIFY(first.size() == 2 && second.size() == 2);

    MjpegStub stub(first);
    QVERIFY(stub.listen());

    FrameBus bus;
    MjpegClient stream;
    StreamStats stats(&stream, &bus);

    QVERIFY(!stats.hasFrame());
    QCOMPARE(stats.frameAgeMs(), 0);

    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
            });

    // StreamStats samples on its own 1 s timer, so assert on the peak reading
    // seen across the window instead of on one manual sample - that makes the
    // check independent of where the timer happens to be in its phase.
    double peak = 0.0;
    connect(&stats, &StreamStats::statsChanged, &stats, [&]() {
        peak = qMax(peak, stats.bitrateBps());
    });

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(stats.hasFrame(), 8000);
    QVERIFY(stats.frameAgeMs() >= 0);

    stats.sample();
    QTest::qWait(300);
    stub.sendMore(second);
    QTest::qWait(1700);

    QVERIFY2(peak > 0.0, "expected a non-zero bitrate once bytes arrived in the window");

    double total = 0.0;
    for (const QByteArray &p : second) {
        total += p.size();
    }
    QVERIFY2(peak >= total / 2.0,
             qPrintable(QStringLiteral("peak %1 B/s is too low for %2 bytes of new data")
                            .arg(peak)
                            .arg(total)));

    stream.stop();
    QTest::qWait(1400);
    stats.sample();
    QCOMPARE(stats.bitrateBps(), 0.0);
}

QTEST_GUILESS_MAIN(TestCapture)
#include "tst_capture.moc"
