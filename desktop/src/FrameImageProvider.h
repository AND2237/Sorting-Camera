#pragma once

#include <QQuickImageProvider>

class FrameBus;

class FrameImageProvider : public QQuickImageProvider
{
public:
    explicit FrameImageProvider(FrameBus *bus);

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    FrameBus *m_bus;
};
