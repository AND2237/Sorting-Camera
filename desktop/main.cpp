#include "src/AppMetrics.h"
#include "src/CameraDevice.h"
#include "src/DeviceRegistry.h"
#include "src/DeviceStatus.h"
#include "src/Diagnostics.h"
#include "src/DiscoveryService.h"
#include "src/FrameBus.h"
#include "src/FrameImageProvider.h"
#include "src/MjpegClient.h"
#include "src/NotificationCenter.h"
#include "src/ProfileEngine.h"
#include "src/Recorder.h"
#include "src/SessionState.h"
#include "src/SnapshotWriter.h"
#include "src/StreamStats.h"
#include "src/UserPreferences.h"

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

    // Registered before any context property can carry it, otherwise the
    // profile engine cannot be exposed to QML as an object pointer.
    qmlRegisterUncreatableType<ProfileEngine>("SortingCamera", 1, 0,
                                             "ProfileEngine", QStringLiteral("owned by its camera"));
    qmlRegisterUncreatableType<NotificationCenter>("SortingCamera", 1, 0, "NotificationCenter",
                                                   QStringLiteral("owned by its camera"));
    qmlRegisterUncreatableType<Diagnostics::Facility>("SortingCamera", 1, 0, "Facility",
                                                      QStringLiteral("one per process"));

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));

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

    // The registry owns every camera. Only one is shown at a time - section 27
    // allows a single active stream in the view - but the objects behind the
    // view are per camera, not per application, so a second camera is a second
    // entry here rather than a second architecture.
    // One facility for the process, installed before anything else logs, so
    // every subsystem's messages are categorised and rate limited from the
    // first line rather than from whenever this object happens to be built.
    Diagnostics::Facility diagnostics;
    diagnostics.setMaxEntries(800);

    DiscoveryService discovery;
    DeviceRegistry registry(&discovery);
    UserPreferences prefs;

    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString captureRoot =
        prefs.captureDirectory().isEmpty()
            ? UserPreferences::defaultCaptureDirectory(pictures)
            : prefs.captureDirectory();

    // Bench runs are scripted against an explicit --host, so they get a device
    // with no discovery behind it.
    CameraDevice *device =
        registry.acquire(QStringLiteral("bench:%1").arg(host), host, controlPort, streamPort);
    registry.setActive(device->deviceId());
    device->status()->startPolling(host, controlPort);

    FrameBus &frameBus = *device->frames();
    MjpegClient &stream = *device->stream();
    DeviceStatus &deviceStatus = *device->status();

    QQmlApplicationEngine engine;
    engine.addImageProvider(
        QStringLiteral("frame"),
        new FrameImageProvider([&registry]() -> FrameBus * {
            CameraDevice *active = registry.activeDevice();
            return active ? active->frames() : nullptr;
        }));

    // Point the QML names at whichever camera is active. Re-declaring a context
    // property re-evaluates the bindings that read it, so the view follows a
    // device switch without QML being rewritten to chase a variable.
    const auto exposeActiveDevice = [&engine, &registry]() {
        CameraDevice *active = registry.activeDevice();
        if (!active) {
            return;
        }
        QQmlContext *ctx = engine.rootContext();
        ctx->setContextProperty(QStringLiteral("frameBus"), active->frames());
        ctx->setContextProperty(QStringLiteral("stream"), active->stream());
        ctx->setContextProperty(QStringLiteral("deviceStatus"), active->status());
        ctx->setContextProperty(QStringLiteral("sessionState"), active->session());
        ctx->setContextProperty(QStringLiteral("recorder"), active->recorder());
        ctx->setContextProperty(QStringLiteral("snapshotWriter"), active->snapshots());
        ctx->setContextProperty(QStringLiteral("streamStats"), active->stats());
        ctx->setContextProperty(QStringLiteral("profiles"), active->profiles());
        ctx->setContextProperty(QStringLiteral("notify"), active->notifications());
        ctx->setContextProperty(QStringLiteral("diagnostics"), Diagnostics::Facility::instance());
    };
    QObject::connect(&registry, &DeviceRegistry::activeDeviceChanged, &app,
                     [&exposeActiveDevice]() { exposeActiveDevice(); });

    engine.rootContext()->setContextProperty(QStringLiteral("registry"), &registry);
    engine.rootContext()->setContextProperty(QStringLiteral("discovery"), &discovery);
    engine.rootContext()->setContextProperty(QStringLiteral("captureRoot"), captureRoot);
    engine.rootContext()->setContextProperty(QStringLiteral("prefs"), &prefs);
    exposeActiveDevice();

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
