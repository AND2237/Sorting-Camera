#include "src/AppMetrics.h"
#include "src/DeviceStatus.h"
#include "src/DiscoveryService.h"
#include "src/FrameBus.h"
#include "src/FrameImageProvider.h"
#include "src/MjpegClient.h"
#include "src/Recorder.h"
#include "src/SessionState.h"
#include "src/SnapshotWriter.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QTimer>

namespace {
} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Basic"));

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("SortingCamera viewer (bench mode records pipeline metrics)"));
    parser.addHelpOption();
    QCommandLineOption benchOpt(QStringLiteral("bench"),
                                QStringLiteral("Run for <seconds> and write metrics, then exit."),
                                QStringLiteral("seconds"));
    QCommandLineOption outOpt(QStringLiteral("out"),
                              QStringLiteral("JSON file for the recorded metrics."),
                              QStringLiteral("file"));
    QCommandLineOption hostOpt(QStringLiteral("host"),
                               QStringLiteral("Camera IP (default 192.168.4.1)."),
                               QStringLiteral("ip"), QStringLiteral("192.168.4.1"));
    QCommandLineOption controlPortOpt(QStringLiteral("control-port"),
                                      QStringLiteral("Control API port (default 80)."),
                                      QStringLiteral("port"), QStringLiteral("80"));
    QCommandLineOption streamPortOpt(QStringLiteral("stream-port"),
                                     QStringLiteral("MJPEG stream port (default 81)."),
                                     QStringLiteral("port"), QStringLiteral("81"));
    QCommandLineOption fsOpt(QStringLiteral("framesize"),
                             QStringLiteral("Request this resolution before the run (qqvga|qvga|vga|svga|xga|hd|sxga|uxga)."),
                             QStringLiteral("key"));
    QCommandLineOption qOpt(QStringLiteral("quality"),
                            QStringLiteral("Request this JPEG quality before the run (0-63)."),
                            QStringLiteral("n"));
    QCommandLineOption warmOpt(QStringLiteral("warmup"),
                               QStringLiteral("Warm-up seconds excluded from the metrics (default 10)."),
                               QStringLiteral("seconds"), QStringLiteral("10"));
    QCommandLineOption fbOpt(QStringLiteral("fb-count"),
                             QStringLiteral("Request this frame-buffer count (1-3) before the run."),
                             QStringLiteral("n"));
    QCommandLineOption grabOpt(QStringLiteral("grab"),
                               QStringLiteral("Request this grab mode (latest|cont) before the run."),
                               QStringLiteral("mode"));
    QCommandLineOption xclkOpt(QStringLiteral("xclk"),
                               QStringLiteral("Request this XCLK in MHz (6-27) before the run."),
                               QStringLiteral("mhz"));
    parser.addOption(benchOpt);
    parser.addOption(outOpt);
    parser.addOption(hostOpt);
    parser.addOption(controlPortOpt);
    parser.addOption(streamPortOpt);
    parser.addOption(fsOpt);
    parser.addOption(qOpt);
    parser.addOption(warmOpt);
    parser.addOption(fbOpt);
    parser.addOption(grabOpt);
    parser.addOption(xclkOpt);
    parser.process(app);

    const bool benchMode = parser.isSet(benchOpt);
    const int benchSeconds = parser.isSet(benchOpt) ? parser.value(benchOpt).toInt() : 0;
    const int warmupSeconds = parser.value(warmOpt).toInt();
    const QString host = parser.value(hostOpt);
    const quint16 controlPort = (quint16)parser.value(controlPortOpt).toUShort();
    const quint16 streamPort = (quint16)parser.value(streamPortOpt).toUShort();
    const QString outPath = parser.value(outOpt);

    FrameBus frameBus;
    MjpegClient stream;
    DeviceStatus deviceStatus;
    DiscoveryService discovery;
    SessionState sessionState;
    sessionState.observe(&stream, &deviceStatus, &discovery);
    Recorder recorder;
    SnapshotWriter snapshotWriter(&frameBus);

    const QString captureRoot =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        + QStringLiteral("/SortingCamera");

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("frame"), new FrameImageProvider(&frameBus));

    QObject::connect(&stream, &MjpegClient::frameReady, &frameBus,
                     [&frameBus, &recorder](const QImage &image, const QByteArray &raw,
                                            qint64 completeMs) {
                         // The bus keeps both views of the same frame, so a
                         // snapshot is always the picture on screen and the
                         // recorder always gets the untouched network bytes.
                         frameBus.setFrame(image, raw, completeMs);
                         if (recorder.isRecording() && !raw.isEmpty()) {
                             recorder.appendFrame(raw, int(recorder.framesWritten()),
                                                  QDateTime::currentMSecsSinceEpoch(),
                                                  image.width(), image.height());
                         }
                     });
    QObject::connect(&stream, &MjpegClient::deviceRecoveryRequested, &deviceStatus,
                     [&deviceStatus, controlPort](const QString &h, quint16) {
                         deviceStatus.startPolling(h, controlPort);
                         deviceStatus.requestCameraRecovery();
                     },
                     Qt::QueuedConnection);

    engine.rootContext()->setContextProperty(QStringLiteral("frameBus"), &frameBus);
    engine.rootContext()->setContextProperty(QStringLiteral("stream"), &stream);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceStatus"), &deviceStatus);
    engine.rootContext()->setContextProperty(QStringLiteral("discovery"), &discovery);
    engine.rootContext()->setContextProperty(QStringLiteral("sessionState"), &sessionState);
    engine.rootContext()->setContextProperty(QStringLiteral("recorder"), &recorder);
    engine.rootContext()->setContextProperty(QStringLiteral("snapshotWriter"),
                                             &snapshotWriter);
    engine.rootContext()->setContextProperty(QStringLiteral("captureRoot"), captureRoot);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("SortingCamera", "Main");

    // Bench runs are scripted against an explicit --host; discovery would only
    // compete for UDP 48888 with the harness that drives them.
    if (!benchMode) {
        discovery.startScanning();
    }

    if (!benchMode) {
        return app.exec();
    }

    const qint64 t_start = AppMetrics::nowMs();
    const QString startedIso = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonObject extra;
    extra["mode"] = QStringLiteral("bench");
    extra["started"] = startedIso;
    extra["host"] = host;
    extra["control_port"] = controlPort;
    extra["stream_port"] = streamPort;
    extra["bench_seconds"] = benchSeconds;
    extra["warmup_seconds"] = warmupSeconds;
    extra["app_version"] = QCoreApplication::applicationVersion();
    if (parser.isSet(fsOpt)) {
        extra["requested_framesize"] = parser.value(fsOpt);
    }
    if (parser.isSet(qOpt)) {
        extra["requested_quality"] = parser.value(qOpt).toInt();
    }
    if (parser.isSet(fbOpt)) {
        extra["requested_fb_count"] = parser.value(fbOpt).toInt();
    }
    if (parser.isSet(grabOpt)) {
        extra["requested_grab"] = parser.value(grabOpt);
    }
    if (parser.isSet(xclkOpt)) {
        extra["requested_xclk_mhz"] = parser.value(xclkOpt).toInt();
    }

    deviceStatus.startPolling(host, controlPort);

    // Configs are applied one at a time, each only after the previous one has
    // finished (configBusy clears). Fixed spacing raced the app's own busy gate
    // and silently dropped a request during the XCLK sweep, which left the
    // device on the previous clock while the run claimed a new one.
    int configSlot = 0;
    int streamScheduled = 0;
    const auto configCount = [&]() {
        int n = 0;
        if (parser.isSet(fsOpt)) ++n;
        if (parser.isSet(qOpt)) ++n;
        if (parser.isSet(xclkOpt)) ++n;
        if (parser.isSet(fbOpt)) ++n;
        if (parser.isSet(grabOpt)) ++n;
        return n;
    }();
    const auto applyNextConfig = [&]() -> bool {
        switch (configSlot) {
        case 0: if (parser.isSet(fsOpt)) { deviceStatus.setResolution(parser.value(fsOpt)); ++configSlot; return true; } return false;
        case 1: if (parser.isSet(qOpt)) { deviceStatus.setQuality(parser.value(qOpt).toInt()); ++configSlot; return true; } return false;
        case 2: if (parser.isSet(xclkOpt)) { deviceStatus.setXclk(parser.value(xclkOpt).toInt()); ++configSlot; return true; } return false;
        case 3: if (parser.isSet(fbOpt)) { deviceStatus.setFrameBufferCount(parser.value(fbOpt).toInt()); ++configSlot; return true; } return false;
        case 4: if (parser.isSet(grabOpt)) { deviceStatus.setGrabMode(parser.value(grabOpt)); ++configSlot; return true; } return false;
        default: return false;
        }
    };
    const auto scheduleStream = [&]() {
        if (streamScheduled) {
            return;
        }
        streamScheduled = 1;
        QTimer::singleShot(1500, &stream, [&stream, host, streamPort]() { stream.start(host, streamPort); });
    };

    QObject::connect(&deviceStatus, &DeviceStatus::configBusyChanged, &app, [&]() {
        if (deviceStatus.configBusy()) {
            return;
        }
        if (configSlot < configCount) {
            QTimer::singleShot(250, &app, [&]() {
                if (!applyNextConfig()) {
                    scheduleStream();
                }
            });
        } else {
            scheduleStream();
        }
    });
    QTimer::singleShot(1200, &app, [&]() {
        if (!applyNextConfig()) {
            scheduleStream();
        }
    });
    QTimer::singleShot(1200 + 2500 * (configCount + 2), &app, scheduleStream);

    QTimer warmupTimer;
    warmupTimer.setSingleShot(true);
    QTimer benchTimer;
    benchTimer.setSingleShot(true);
    bool metricsStarted = false;
    bool warmupArmed = false;
    qint64 metricsStartMs = -1;
    QObject::connect(&warmupTimer, &QTimer::timeout, [&]() {
        AppMetrics::instance().reset();
        metricsStarted = true;
        metricsStartMs = AppMetrics::nowMs() - t_start;
        qInfo("[bench] warmup done, measuring for %d s", benchSeconds);
        benchTimer.start(benchSeconds * 1000);
    });
    QObject::connect(&benchTimer, &QTimer::timeout, [&]() {
        const qint64 wall = AppMetrics::nowMs() - t_start;
        extra["total_wall_ms"] = wall;
        extra["device_status"] = deviceStatus.status();
        extra["cpu_percent"] = AppMetrics::instance().processCpuPercent();
        extra["working_set_bytes"] = double(AppMetrics::instance().workingSetBytes());
        extra["finished"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        extra["metrics_started"] = metricsStarted;
        extra["metrics_start_ms"] = metricsStartMs;
        extra["metrics_end_ms"] = AppMetrics::nowMs() - t_start;
        extra["app_started_iso"] = startedIso;
        extra["config_error"] = deviceStatus.configError();

        if (!outPath.isEmpty()) {
            const bool ok = AppMetrics::instance().writeJson(outPath, extra);
            qInfo("[bench] wrote %s (%s)", qPrintable(outPath), ok ? "ok" : "FAILED");
        }
        stream.stop();
        QCoreApplication::exit(0);
    });
    QObject::connect(&stream, &MjpegClient::frameReady, &app,
                     [&warmupTimer, warmupSeconds, &warmupArmed](const QImage &, const QByteArray &,
                                                                 qint64) {
                         if (!warmupArmed) {
                             warmupArmed = true;
                             warmupTimer.start(warmupSeconds * 1000);
                         }
                     },
                     Qt::QueuedConnection);
    QTimer::singleShot((benchSeconds + warmupSeconds + 60) * 1000, &app, [&]() {
        if (!metricsStarted) {
            qWarning("[bench] no frames received; writing what was collected");
            benchTimer.start(1);
        }
    });

    return app.exec();
}
