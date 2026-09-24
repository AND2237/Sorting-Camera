#include "FrameImageProvider.h"

#include "FrameBus.h"

FrameImageProvider::FrameImageProvider(FrameBus *bus)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_bus(bus)
{
}

QImage FrameImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(id)

    QImage img = m_bus ? m_bus->image() : QImage();
    if (img.isNull()) {
        img = QImage(640, 360, QImage::Format_RGB32);
        img.fill(QColor(0x14, 0x18, 0x1d));
    }

    if (size) {
        *size = img.size();
    }
    if (requestedSize.isValid() && !requestedSize.isEmpty()) {
        return img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return img;
}
