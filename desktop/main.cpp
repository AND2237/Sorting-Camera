#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "src/DeviceStatus.h"
#include "src/FrameBus.h"
#include "src/FrameImageProvider.h"
#include "src/MjpegClient.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Basic"));

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationName(QStringLiteral("SortingCamera"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    FrameBus frameBus;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("frame"), new FrameImageProvider(&frameBus));

    MjpegClient stream;
    DeviceStatus deviceStatus;

    QObject::connect(&stream, &MjpegClient::frameReady, &frameBus, &FrameBus::setFrame);

    engine.rootContext()->setContextProperty(QStringLiteral("frameBus"), &frameBus);
    engine.rootContext()->setContextProperty(QStringLiteral("stream"), &stream);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceStatus"), &deviceStatus);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("SortingCamera", "Main");

    const int rc = app.exec();

    stream.stop();
    deviceStatus.stopPolling();
    return rc;
}
