#include "Capabilities.h"

#include <QAbstractSocket>
#include <QGuiApplication>
#include <QInputDevice>
#include <QNetworkInterface>
#include <QPointingDevice>
#include <QScreen>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QVariantMap>

#include <QQuickWindow>
#include <QSGRendererInterface>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

QVariantMap entry(const QString &label, const QString &value)
{
    QVariantMap m;
    m.insert(QStringLiteral("label"), label);
    m.insert(QStringLiteral("value"), value);
    return m;
}

QString cpuName()
{
#ifdef Q_OS_WIN
    // The brand string Windows itself reports for the boot processor. A registry
    // read rather than a CPUID intrinsic: portable across toolchains and it
    // already reflects what the machine is called.
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
                  QSettings::NativeFormat);
    const QString name = reg.value(QStringLiteral("ProcessorNameString")).toString();
    if (!name.isEmpty()) {
        return name.trimmed();
    }
#endif
    return QSysInfo::currentCpuArchitecture();
}

// The scene graph backend by name once a window exists; before that, the API
// this build will ask for. "software" and "null" mean no hardware path.
QString sceneGraphApiName()
{
    const QString backend = QQuickWindow::sceneGraphBackend();
    if (!backend.isEmpty()) {
        return backend;
    }
    switch (QQuickWindow::graphicsApi()) {
    case QSGRendererInterface::Software:
        return QStringLiteral("software");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("opengl");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("d3d11");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("d3d12");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("null");
    default:
        return QStringLiteral("default");
    }
}

bool touchPresent()
{
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    for (const QInputDevice *device : devices) {
        if (!device) {
            continue;
        }
        if (device->type() == QInputDevice::DeviceType::TouchScreen
            || device->type() == QInputDevice::DeviceType::TouchPad) {
            return true;
        }
    }
    const QPointingDevice *primary = QPointingDevice::primaryPointingDevice();
    return primary && primary->type() == QInputDevice::DeviceType::TouchScreen;
}

} // namespace

Capabilities::Capabilities(QObject *parent)
    : QObject(parent)
{
    probe();
}

void Capabilities::refresh()
{
    probe();
}

void Capabilities::probe()
{
    QVariantList out;
    m_hardwareGraphics = false;
    m_touch = false;
    m_onAp = false;

    // 1. CPU characteristics.
    const int cores = QThread::idealThreadCount();
    out << entry(QStringLiteral("CPU"),
                 QStringLiteral("%1 (%2), %3 logical cores")
                     .arg(cpuName(),
                          QSysInfo::buildCpuArchitecture(),
                          cores > 0 ? QString::number(cores) : QStringLiteral("unknown")));

    // 2. Available memory.
    QString memory = QStringLiteral("unknown (not probed on this platform)");
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        const auto gib = [](quint64 bytes) {
            return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1);
        };
        memory = QStringLiteral("%1 GiB physical, %2 GiB available")
                     .arg(gib(status.ullTotalPhys), gib(status.ullAvailPhys));
    }
#endif
    out << entry(QStringLiteral("Memory"), memory);

    // 3. Graphics backend.
    out << entry(QStringLiteral("Graphics backend"),
                 QStringLiteral("%1 platform, scene graph %2")
                     .arg(QGuiApplication::platformName(), sceneGraphApiName()));

    // 4. Graphics acceleration availability. QT_QUICK_BACKEND=software is the
    //    documented override, and an explicit "software"/"null" backend is the
    //    fallback §28 asks for: the app keeps rendering, just without acceleration.
    const QString forced = QString::fromLocal8Bit(qgetenv("QT_QUICK_BACKEND"));
    const QString api = sceneGraphApiName();
    m_hardwareGraphics = !forced.contains(QStringLiteral("software"))
        && !api.contains(QStringLiteral("software")) && !api.contains(QStringLiteral("null"));
    out << entry(QStringLiteral("Graphics acceleration"),
                 m_hardwareGraphics
                     ? QStringLiteral("hardware path (%1)").arg(api)
                     : QStringLiteral("software fallback (%1) - rendering still works, slower")
                           .arg(api));

    // 5. Video decode acceleration availability. Honest answer for this build:
    //    JPEG frames are decoded by Qt's own image reader, on the CPU. There is
    //    no hardware decode path to detect, and the software path is the fallback.
    out << entry(QStringLiteral("Video decode acceleration"),
                 QStringLiteral("none - JPEG is decoded by Qt (QImage) on the CPU; "
                                "this build has no hardware decode path"));

    // 6. Display resolution.
    QString display = QStringLiteral("unknown (no screen attached)");
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect geometry = screen->geometry();
        const QString identity = screen->manufacturer().isEmpty()
            ? screen->name()
            : QStringLiteral("%1 %2").arg(screen->manufacturer(), screen->model());
        display = QStringLiteral("%1x%2 @ %3 Hz, %4 logical dpi%5")
                      .arg(geometry.width())
                      .arg(geometry.height())
                      .arg(screen->refreshRate(), 0, 'f', 0)
                      .arg(screen->logicalDotsPerInch(), 0, 'f', 0)
                      .arg(identity.isEmpty() ? QString() : QStringLiteral(", %1").arg(identity));
    }
    out << entry(QStringLiteral("Display"), display);

    // 7. Network interfaces. One entry per up IPv4 interface; the camera's own
    //    softAP prefix is flagged because "the PC never joined the AP" is the
    //    most common reason a viewer sees nothing.
    int reported = 0;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        QStringList addresses;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &address : entries) {
            if (address.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                addresses << address.ip().toString();
            }
        }
        if (addresses.isEmpty()) {
            continue;
        }
        const QString joined = addresses.join(QStringLiteral(", "));
        if (joined.contains(QStringLiteral("192.168.4."))) {
            m_onAp = true;
        }
        const QString name = iface.humanReadableName().isEmpty()
            ? iface.name()
            : iface.humanReadableName();
        out << entry(QStringLiteral("Network interface"),
                     QStringLiteral("%1: %2%3")
                         .arg(name, joined,
                              m_onAp && joined.contains(QStringLiteral("192.168.4."))
                                  ? QStringLiteral(" (camera softAP)")
                                  : QString()));
        ++reported;
    }
    if (reported == 0) {
        out << entry(QStringLiteral("Network interface"),
                     QStringLiteral("no IPv4 interface is up"));
    }

    // 8. Touch capability where available.
    m_touch = touchPresent();
    out << entry(QStringLiteral("Touch"),
                 m_touch ? QStringLiteral("touchscreen or touchpad present")
                         : QStringLiteral("none detected (mouse/keyboard only)"));

    // Context, not a §28 bullet, but the first thing anyone asks when a
    // capability looks wrong.
    out << entry(QStringLiteral("System"), QSysInfo::prettyProductName());

    m_entries = out;
    emit entriesChanged();
}
