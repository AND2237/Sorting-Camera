#include "FrameImageProvider.h"

#include "AppMetrics.h"
#include "FrameBus.h"

FrameImageProvider::FrameImageProvider(FrameBus *bus)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_bus(bus)
{
}

QImage FrameImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(id)

    const qint64 t0 = AppMetrics::nowUs();

    QImage img = m_bus ? m_bus->image() : QImage();
    const qint64 completeMs = m_bus ? m_bus->lastFrameCompleteMs() : 0;
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
