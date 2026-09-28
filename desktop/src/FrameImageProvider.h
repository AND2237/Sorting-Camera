#pragma once

#include <QQuickImageProvider>

#include <functional>

class FrameBus;

// The QML image provider for the live view. It resolves the bus per request
// rather than holding one, because the bus belongs to whichever camera is
// active and that can change while the window is open. Resolving here is what
// lets the picture follow a device switch without the QML side rebuilding the
// image source.
class FrameImageProvider : public QQuickImageProvider
{
public:
    using Resolver = std::function<FrameBus *()>;

    explicit FrameImageProvider(Resolver resolver);
    explicit FrameImageProvider(FrameBus *bus);

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    Resolver m_resolver;
};
