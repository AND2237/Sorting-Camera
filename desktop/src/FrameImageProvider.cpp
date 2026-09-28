#include "FrameImageProvider.h"

#include "AppMetrics.h"
#include "FrameBus.h"

FrameImageProvider::FrameImageProvider(Resolver resolver)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_resolver(std::move(resolver))
{
}

FrameImageProvider::FrameImageProvider(FrameBus *bus)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_resolver([bus]() { return bus; })
{
}

QImage FrameImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(id)

    const qint64 t0 = AppMetrics::nowUs();

    // Resolved per request: the active camera can change between two frames.
    FrameBus *bus = m_resolver ? m_resolver() : nullptr;

    QImage img = bus ? bus->image() : QImage();
    const qint64 completeMs = bus ? bus->lastFrameCompleteMs() : 0;
    if (img.isNull()) {
        img = QImage(640, 360, QImage::Format_RGB32);
        img.fill(QColor(0x14, 0x18, 0x1d));
    }

    if (size) {
        *size = img.size();
    }
    if (requestedSize.isValid() && !requestedSize.isEmpty()) {
        const qint64 t_scale = AppMetrics::nowUs();
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        AppMetrics::instance().addScaleUs(AppMetrics::nowUs() - t_scale);
    }

    if (completeMs > 0) {
        AppMetrics::instance().addRenderAgeMs(AppMetrics::nowMs() - completeMs);
    }
    AppMetrics::instance().addRenderUs(AppMetrics::nowUs() - t0);
    return img;
}
