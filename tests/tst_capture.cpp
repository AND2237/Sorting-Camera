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
#include <QThread>

#include <atomic>

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

    // Accepts the connection and answers with nothing. The worker's first-byte
    // watchdog is the only thing that produces "no response from camera", and
    // that is the only error that feeds the recovery budget - so a test that
    // wants to spend that budget has to starve it like a wedged camera does.
    bool silent = false;

    // Fails the parser with a part header block that carries no usable
    // Content-Length. Deliberately NOT a first-byte timeout: it stops the
    // streak climbing so the next episode has to earn its way back to two,
    // which is what separates "re-arms after a healthy frame" from a counter
    // that just never stops counting.
    void sendBrokenPart()
    {
        if (!m_socket) {
            return;
        }
        m_socket->write("--FRAME\r\nContent-Length: nope\r\n\r\n");
        m_socket->flush();
    }

    // The RFC 2046 close-delimiter the firmware now sends when the stream ends
    // (FW-9). Off by default so every other test still sees the old shape of
    // "the body just stops", which is the case it is actually about.
    bool sendCloseDelimiter = false;

    // CP-8: the firmware writes every part with httpd_resp_send_chunk, so the
    // device's stream is Transfer-Encoding: chunked - a shape no test used to
    // produce, which left the whole ChunkSize/ChunkData/ChunkEnd path dead to
    // CI. The body is framed the way a chunked server writes it, and split
    // *inside* the Content-Length digits of each part, because a chunked body
    // that happens to parse identically as identity would prove nothing.
    bool chunked = false;

    // CP-9: the whole response, status line included, written exactly as the
    // test spelled it. A malformed-input case is then a byte string rather
    // than another stub with another knob.
    QByteArray rawResponse;

    // Pushes more parts onto an already-open connection, so a test can create
    // traffic inside a measurement window rather than only before it.
    void sendMore(const QList<QByteArray> &payloads)
    {
        if (!m_socket) {
            return;
        }
        m_socket->write(chunked ? encodeChunked(payloads) : encode(payloads));
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

    static QByteArray chunk(const QByteArray &piece)
    {
        return QByteArray::number(piece.size(), 16) + "\r\n" + piece + "\r\n";
    }

    static QByteArray encodeChunked(const QList<QByteArray> &payloads)
    {
        QByteArray out;
        for (const QByteArray &p : payloads) {
            const QByteArray head = "--FRAME\r\nContent-Length: "
                                    + QByteArray::number(p.size()) + "\r\n\r\n";
            // One digit into the number: an identity reader would then see
            // "Content-Length: 1" followed by a chunk-size line, take one byte
            // as the whole frame and fail to decode it.
            const int cut = head.indexOf("Content-Length: ") + 17;
            out += chunk(head.left(cut));
            out += chunk(head.mid(cut));
            out += chunk(p);
        }
        out += "0\r\n\r\n";
        return out;
    }

    void onConnection()
    {
        QTcpSocket *sock = m_server.nextPendingConnection();
        m_socket = sock;
        // Every connection states its own request, so a later connection is
        // judged on what it actually asked for rather than on what the one
        // before it left behind.
        m_request.clear();
        m_sent = false;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            m_request.append(sock->readAll());
            if (silent || m_sent || !m_request.contains("\r\n\r\n")) {
                return;
            }
            m_sent = true;
            if (!rawResponse.isEmpty()) {
                sock->write(rawResponse);
                sock->flush();
                return;
            }
            QByteArray out;
            out += "HTTP/1.1 200 OK\r\n";
            out += "Content-Type: multipart/x-mixed-replace; boundary=--FRAME\r\n";
            out += "Cache-Control: no-cache\r\n";
            if (chunked) {
                out += "Transfer-Encoding: chunked\r\n";
            }
            out += "\r\n";
            out += chunked ? encodeChunked(m_payloads) : encode(m_payloads);
            if (sendCloseDelimiter && !chunked) {
                out += "\r\n--FRAME--\r\n";
            }
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
    void framesAreDeliveredOffTheGuiThread();
    void aCloseDelimiterEndsTheBodyCleanly();
    void recoveryBudgetRearmsAfterTheStreamIsHealthyAgain();
    void theRetryLadderSpendsEveryStepThenGivesUp();
    void aDecodedFrameResetsTheRetryLadder();
    void aChunkedStreamDecodesTheBytesTheFirmwareSends();
    void malformedStreamsFailWithTheNamedError();
    void aForeignBoundaryNeverMatchesAndTheStallWatchdogEndsTheStream();
    void liveCameraBytesAreStoredVerbatim();
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

// The stub above proves the parser and the writers agree. It cannot prove
// that the camera's own bytes survive, which is the rule AGENTS.md states
// outright: a snapshot and a recording hold the exact JPEG that arrived, and
// are never decode-then-re-encode. This runs the same writers against the real
// device on the stream port, which is deliberately unauthenticated, so the
// check needs no credentials and no GUI. Gated like the other live-device
// tests so a normal ctest run stays hermetic.
void TestCapture::liveCameraBytesAreStoredVerbatim()
{
    const QByteArray host = qgetenv("SCAM_TEST_HOST");
    if (host.isEmpty()) {
        QSKIP("set SCAM_TEST_HOST to run the live byte-identity test");
    }
    const int portEnv = qEnvironmentVariableIntValue("SCAM_STREAM_PORT");
    const quint16 streamPort = portEnv > 0 ? quint16(portEnv) : 81;

    FrameBus bus;
    MjpegClient stream;
    SnapshotWriter writer(&bus);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QList<QByteArray> received;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus, &received](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
                if (!raw.isEmpty()) {
                    received.append(raw);
                }
            });

    stream.start(QString::fromLocal8Bit(host), streamPort);
    QTRY_VERIFY_WITH_TIMEOUT(received.size() >= 5, 20000);

    // What the camera actually put on the wire: a complete JPEG, not a
    // fragment and not something the desktop reshaped on the way past.
    for (const QByteArray &raw : received) {
        QVERIFY(raw.size() > 1024);
        QCOMPARE(raw.left(2), QByteArray("\xFF\xD8", 2));  // SOI
        QCOMPARE(raw.right(2), QByteArray("\xFF\xD9", 2)); // EOI
    }
    QVERIFY2(!bus.image().isNull(), "the live frame did not decode");
    const QSize liveSize = bus.image().size();
    QVERIFY2(liveSize.width() >= 640 && liveSize.height() >= 480,
             qPrintable(QStringLiteral("unexpected live frame size %1x%2")
                            .arg(liveSize.width())
                            .arg(liveSize.height())));

    // Snapshot with the stream stopped, so the frame the writer saved is known
    // exactly and the comparison cannot race a new arrival.
    stream.stop();
    const QByteArray held = bus.rawFrame();
    QVERIFY(!held.isEmpty());
    const QString shot = writer.save(dir.path(), QStringLiteral("livedev"));
    QVERIFY2(!shot.isEmpty(), qPrintable(writer.errorString()));

    QFile shotFile(shot);
    QVERIFY(shotFile.open(QIODevice::ReadOnly));
    const QByteArray written = shotFile.readAll();
    QCOMPARE(written, held);
    QCOMPARE(sha256(written), sha256(held));

    // And that the saved file is the original rather than a re-encode of it.
    // Any re-encode, at any quality, produces different bytes; matching the
    // source hash is what "never re-encoded" actually means on disk.
    QByteArray reencoded;
    QBuffer buf(&reencoded);
    buf.open(QIODevice::WriteOnly);
    QVERIFY(bus.image().save(&buf, "JPG", 90));
    QVERIFY2(sha256(reencoded) != sha256(written),
             "the snapshot is a re-encode of the decoded frame, not the bytes received");

    // The recording path over the same live device.
    Recorder recorder;
    QList<QByteArray> recorded;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus, &recorder, &recorded](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
                if (recorder.isRecording() && !raw.isEmpty()) {
                    recorder.appendFrame(raw, int(recorder.framesWritten()),
                                         QDateTime::currentMSecsSinceEpoch(), img.width(),
                                         img.height());
                    recorded.append(raw);
                }
            });

    QJsonObject meta;
    meta[QStringLiteral("device_id")] = QStringLiteral("live");
    QVERIFY2(recorder.start(dir.path(), meta), qPrintable(recorder.errorString()));
    stream.start(QString::fromLocal8Bit(host), streamPort);
    QTRY_VERIFY_WITH_TIMEOUT(recorded.size() >= 5, 20000);
    recorder.stop();
    stream.stop();

    QVector<Recorder::FrameEntry> entries;
    QString err;
    QVERIFY2(Recorder::scan(recorder.path(), &entries, nullptr, &err), qPrintable(err));
    QCOMPARE(entries.size(), recorded.size());

    QFile rec(recorder.path());
    QVERIFY(rec.open(QIODevice::ReadOnly));
    for (int i = 0; i < entries.size(); i++) {
        QVERIFY(rec.seek(entries.at(i).offset + 4));
        const QByteArray got = rec.read(entries.at(i).bytes);
        QCOMPARE(got, recorded.at(i));
        QVERIFY2(received.contains(got),
                 "a recorded frame is not among the bytes the camera sent");
    }
}

// CP-3: the worker-to-client hop used to be delivered by an Auto connection,
// which queued to MjpegClient's GUI affinity, so frameReady was emitted from
// the GUI thread and every consumer of it - FrameBus and Recorder included -
// was driven from there too. The delivery thread is observable, so this
// asserts it directly rather than inferring it from locking. The connection
// is deliberately Direct: that makes the lambda run where the signal is
// emitted, which is the thread under test.
void TestCapture::framesAreDeliveredOffTheGuiThread()
{
    const QList<QByteArray> payloads = realJpegs(3);
    QVERIFY(payloads.size() == 3);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());

    QThread *const guiThread = QThread::currentThread();
    std::atomic<bool> delivered{false};
    std::atomic<bool> onGuiThread{true};

    MjpegClient stream;
    // Qt refuses a nullptr context, so the sender is used as one. With
    // DirectConnection the context object's affinity is irrelevant - the
    // lambda runs in the thread that emits the signal, which is the thread
    // under test.
    connect(&stream, &MjpegClient::frameReady, &stream,
            [&](const QImage &, const QByteArray &, qint64) {
                onGuiThread.store(QThread::currentThread() == guiThread);
                delivered.store(true);
            },
            Qt::DirectConnection);

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(delivered.load(), 8000);
    stream.stop();

    QVERIFY2(!onGuiThread.load(),
             "frameReady is emitted on the GUI thread, which is where CP-3 "
             "puts the frame bus and the recorder");
}

// FW-9: the MJPEG stream used to stop between two boundaries, which only
// worked because MjpegClient keys on the literal "--FRAME" rather than on a
// parsed delimiter. The firmware now closes the multipart body with the RFC
// 2046 close-delimiter a conforming reader expects, and this pins the client
// side of that change: the terminator is not a fifth part, it does not corrupt
// the parser, and the frame still on screen is still the last real one. The
// firmware side of FW-9 cannot be exercised without hardware, so what is
// asserted here is the contract the firmware now relies on.
void TestCapture::aCloseDelimiterEndsTheBodyCleanly()
{
    const QList<QByteArray> payloads = realJpegs(4);
    QVERIFY(payloads.size() == 4);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    stub.sendCloseDelimiter = true;

    FrameBus bus;
    MjpegClient stream;
    connect(&stream, &MjpegClient::frameReady, &bus,
            [&bus](const QImage &img, const QByteArray &raw, qint64 ms) {
                bus.setFrame(img, raw, ms);
            });

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(bus.version() >= payloads.size(), 8000);

    // The close-delimiter is a terminator, not a part: nothing further decodes,
    // the parser reports no failure (a malformed tail would populate
    // errorString and start the reconnect ladder), and byte identity holds.
    QTest::qWait(600);
    QCOMPARE(bus.version(), payloads.size());
    QCOMPARE(stream.errorString(), QString());
    QCOMPARE(sha256(bus.rawFrame()), sha256(payloads.last()));
    stream.stop();
}

// CP-5: the recovery budget was incremented and compared but never assigned
// back anywhere, so two recovery requests in a process lifetime permanently
// disabled self-repair. A camera that ran cleanly for an hour and then wedged
// got nothing.
//
// The budget is 2, so a test with two episodes proves nothing: the second
// recovery is allowed either way. It takes a third - which is only reachable
// if the budget was handed back after each healthy frame - to tell the fix
// from the defect. Each episode costs two first-byte timeouts and that
// watchdog is 6 s in production; the test does not shorten it, because it is
// exercising the shipped constants, so it runs for roughly 40 s on purpose.
void TestCapture::recoveryBudgetRearmsAfterTheStreamIsHealthyAgain()
{
    const QList<QByteArray> payloads = realJpegs(2);
    QVERIFY(payloads.size() == 2);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    stub.silent = true;

    MjpegClient stream;
    int recoveries = 0;
    int frames = 0;
    // Both queued to this thread, so neither counter is ever touched off it.
    connect(&stream, &MjpegClient::deviceRecoveryRequested, &stream,
            [&recoveries]() { ++recoveries; }, Qt::QueuedConnection);
    connect(&stream, &MjpegClient::frameReady, &stream, [&frames]() { ++frames; },
            Qt::QueuedConnection);

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    for (int episode = 1; episode <= 3; ++episode) {
        QTRY_VERIFY_WITH_TIMEOUT(recoveries == episode, 40000);
        QCOMPARE(stream.noResponseStreak(), 2);
        if (episode == 3) {
            break;
        }

        // A real camera answers the re-init request by coming back. So does
        // the stub, and the frame it then produces is what hands the budget
        // back - without it episode three can never be reached.
        stub.silent = false;
        const int framesBefore = frames;
        QTRY_VERIFY_WITH_TIMEOUT(frames > framesBefore, 20000);
        QCOMPARE(stream.noResponseStreak(), 0);

        // Take the stream away with a failure that is not a first-byte
        // timeout, so the streak starts from zero rather than carrying over,
        // and starve the next connection so the episode has to earn its way
        // back to two.
        stub.sendBrokenPart();
        stub.silent = true;
    }

    QCOMPARE(recoveries, 3);
    stream.stop();
}

// CP-7: the reconnect ladder is what keeps a dead camera from masquerading as
// a live stream (§24, "no reconnect storms"). The implementation was correct
// and nothing exercised it: a regression that advanced the counter on TCP
// connect, or that shipped a different delay table, went green. Every constant
// asserted here is the shipped one - the test does not shorten them, it just
// makes each attempt fail immediately instead of after a watchdog.
void TestCapture::theRetryLadderSpendsEveryStepThenGivesUp()
{
    const QList<QByteArray> payloads = realJpegs(2);
    QVERIFY(payloads.size() == 2);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    // Response headers that never terminate, so every attempt fails for the
    // same reason and as fast as the parser allows.
    stub.rawResponse = "HTTP/1.1 200 OK\r\n" + QByteArray(9000, 'X');

    MjpegClient stream;
    QList<int> attempts;
    QList<int> delays;
    connect(&stream, &MjpegClient::retryAttemptChanged, &stream, [&]() {
        attempts.append(stream.retryAttempt());
        delays.append(stream.retryDelayMs());
    });

    QCOMPARE(stream.maxRetries(), 5);
    // The shipped ladder: five retries at 500/1000/2000/3000/5000 ms after the
    // initial attempt, so six connections in total.
    const QList<int> expectedDelays{500, 1000, 2000, 3000, 5000};
    stream.start(QStringLiteral("127.0.0.1"), stub.port());

    QTRY_VERIFY_WITH_TIMEOUT(stream.retryAttempt() == 5, 30000);
    // Five retries are scheduled by the first five failures; it is the sixth
    // connection's failure - the one after the 5000 ms delay - that must not
    // schedule a seventh. If it had, reconnecting would still be true and the
    // reason would have been cleared for the next attempt.
    QTRY_VERIFY(!stream.isReconnecting());
    QTRY_VERIFY(!stream.lastErrorString().isEmpty());
    QVERIFY2(stream.errorString().contains(QStringLiteral("malformed HTTP response headers")),
             qPrintable(stream.errorString()));
    QTest::qWait(600);
    QCOMPARE(stream.retryAttempt(), 5);
    QCOMPARE(attempts.size(), expectedDelays.size());
    QCOMPARE(delays.size(), expectedDelays.size());
    for (int i = 0; i < expectedDelays.size(); ++i) {
        QCOMPARE(attempts.at(i), i + 1);
        QCOMPARE(delays.at(i), expectedDelays.at(i));
    }
    stream.stop();
}

// The other half of CP-7: the ladder must rewind when a frame actually
// arrives, or a camera that hiccuped once would be written off after five
// hiccups spread over a session. The frame is the only thing that hands the
// ladder back, so the frame is what the test uses.
void TestCapture::aDecodedFrameResetsTheRetryLadder()
{
    const QList<QByteArray> payloads = realJpegs(2);
    QVERIFY(payloads.size() == 2);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    stub.rawResponse = "HTTP/1.1 200 OK\r\n" + QByteArray(9000, 'X');

    MjpegClient stream;
    int frames = 0;
    connect(&stream, &MjpegClient::frameReady, &stream, [&frames]() { ++frames; },
            Qt::QueuedConnection);

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(stream.retryAttempt() == 1, 8000);
    QCOMPARE(stream.retryDelayMs(), 500);

    // The next attempt, one delay later, is answered properly. A healthy frame
    // has to put the ladder back to step zero.
    stub.rawResponse = QByteArray();
    QTRY_VERIFY_WITH_TIMEOUT(frames > 0, 8000);
    QTRY_COMPARE(stream.retryAttempt(), 0);

    // ...and the failure after that starts from step one again rather than
    // carrying the earlier attempt forward.
    stub.sendBrokenPart();
    QTRY_VERIFY_WITH_TIMEOUT(stream.retryAttempt() == 1, 8000);
    QCOMPARE(stream.retryDelayMs(), 500);
    QVERIFY2(stream.lastErrorString().contains(QStringLiteral("Content-Length")),
             qPrintable(stream.lastErrorString()));
    stream.stop();
}

// CP-8: the stub used to emit Content-Length parts with no Transfer-Encoding,
// so the parser ran its Identity branch and the chunked states - the ones the
// real device uses on every frame, because httpd_resp_send_chunk is chunked -
// were executed by nothing. The split inside the Content-Length digits is what
// makes this discriminating: an identity reader would take one byte as the
// whole frame and fail to decode it.
void TestCapture::aChunkedStreamDecodesTheBytesTheFirmwareSends()
{
    const QList<QByteArray> payloads = realJpegs(4);
    QVERIFY(payloads.size() == 4);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    stub.chunked = true;

    MjpegClient stream;
    QList<QByteArray> raws;
    connect(&stream, &MjpegClient::frameReady, &stream,
            [&raws](const QImage &, const QByteArray &raw, qint64) { raws.append(raw); },
            Qt::QueuedConnection);

    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_COMPARE_WITH_TIMEOUT(stream.framesReceived(), 4u, 8000);
    stream.stop();

    QCOMPARE(stream.framesDropped(), 0u);
    QVERIFY2(stream.lastErrorString().isEmpty(), qPrintable(stream.lastErrorString()));
    QCOMPARE(raws.size(), payloads.size());
    for (int i = 0; i < payloads.size(); ++i) {
        QCOMPARE(sha256(raws.value(i)), sha256(payloads.at(i)));
    }
}

// CP-9: the bounds in MjpegClient (8 MiB of buffer, 8 KiB of response headers,
// 2 KiB of part headers, 128 B of chunk line) were verified by reading the
// code and then had no regression guard at all. Each case is the byte string
// that should trip one bound, and the assertion is on the reason the parser
// gives - which is what proves the right branch fired rather than any branch.
void TestCapture::malformedStreamsFailWithTheNamedError()
{
    const QList<QByteArray> payloads = realJpegs(1);
    QVERIFY(payloads.size() == 1);

    const QByteArray okHead =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--FRAME\r\n"
        "Cache-Control: no-cache\r\n\r\n";
    const QByteArray chunkHead =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--FRAME\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";

    struct Case
    {
        const char *name;
        QByteArray response;
        const char *expect;
    };
    const QList<Case> cases{
        {"response headers over budget",
         "HTTP/1.1 200 OK\r\n" + QByteArray(9000, 'X'),
         "malformed HTTP response headers"},
        {"part headers over budget",
         okHead + "--FRAME\r\n" + QByteArray(3000, 'X'),
         "malformed part headers"},
        {"chunk size line over budget",
         chunkHead + QByteArray(200, 'A'),
         "malformed chunk size line"},
        {"non-numeric chunk size", chunkHead + "xyz\r\n", "bad chunk size"},
        {"chunk terminator that is not CRLF", chunkHead + "5\r\nHELLOXY",
         "malformed chunk terminator"},
        {"garbage Content-Length",
         okHead + "--FRAME\r\nContent-Length: nope\r\n\r\n",
         "missing or invalid Content-Length"},
        {"part over the 8 MiB buffer budget",
         okHead + "--FRAME\r\nContent-Length: 9000000\r\n\r\n"
                 + QByteArray(8 * 1024 * 1024 + 512 * 1024, 'Z'),
         "stream buffer overflow"},
    };

    for (const Case &c : cases) {
        MjpegStub stub(payloads);
        QVERIFY2(stub.listen(), c.name);
        stub.rawResponse = c.response;

        MjpegClient stream;
        stream.start(QStringLiteral("127.0.0.1"), stub.port());
        QTRY_VERIFY2_WITH_TIMEOUT(
            stream.lastErrorString().contains(QString::fromUtf8(c.expect)),
            qPrintable(QStringLiteral("%1: got \"%2\"")
                           .arg(QString::fromUtf8(c.name), stream.lastErrorString())),
            10000);
        stream.stop();
        QTest::qWait(50);
    }
}

// CP-9's boundary case and the stall watchdog in one: the boundary parameter
// is deliberately ignored (the firmware writes --FRAME, and a reader that
// trusted a camera-supplied boundary would be handing a remote string to a
// parser), so a stream that declares another one matches nothing. What has to
// happen then is bounded and finite - the 8 s stall watchdog ends it - rather
// than a hang or an unbounded buffer.
void TestCapture::aForeignBoundaryNeverMatchesAndTheStallWatchdogEndsTheStream()
{
    const QList<QByteArray> payloads = realJpegs(1);
    QVERIFY(payloads.size() == 1);

    MjpegStub stub(payloads);
    QVERIFY(stub.listen());
    stub.rawResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=NOTFRAME\r\n\r\n"
        + QByteArray(500, 'q');

    MjpegClient stream;
    stream.start(QStringLiteral("127.0.0.1"), stub.port());
    QTRY_VERIFY_WITH_TIMEOUT(
        stream.lastErrorString().contains(QStringLiteral("stream stalled")), 15000);
    QVERIFY2(stream.lastErrorString().contains(QStringLiteral("8000 ms")),
             qPrintable(stream.lastErrorString()));
    stream.stop();

    // Nothing decoded and nothing discarded: the data never became a part.
    QCOMPARE(stream.framesReceived(), 0u);
    QCOMPARE(stream.framesDropped(), 0u);
    QVERIFY(stream.bytesReceived() > 0);
}

QTEST_GUILESS_MAIN(TestCapture)
#include "tst_capture.moc"
