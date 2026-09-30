#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

// Master Prompt §28 (gap G-1): the final HMI hardware is unknown, so the app
// determines the system's capabilities at startup instead of assuming a dev
// machine. Detection and reporting only - §28's "use capability-based
// behavior" is deliberately *not* done here: nothing in the app may branch on
// these values yet, so the probe can be wrong about an exotic machine without
// changing what the app does. The values are surfaced in the F12 diagnostics
// panel, where an operator can read them.
//
// Every probe has a fallback. A capability that cannot be determined reports
// why ("unknown on this platform") rather than a plausible-looking number -
// a fabricated figure would be worse than an admitted gap, because §28 exists
// to stop this code from guessing.
class Capabilities : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(bool hardwareGraphics READ hardwareGraphics NOTIFY entriesChanged)
    Q_PROPERTY(bool touch READ touch NOTIFY entriesChanged)
    Q_PROPERTY(bool onCameraAccessPoint READ onCameraAccessPoint NOTIFY entriesChanged)

public:
    explicit Capabilities(QObject *parent = nullptr);

    // One {label, value} map per §28 bullet, in §28 order.
    QVariantList entries() const { return m_entries; }
    bool hardwareGraphics() const { return m_hardwareGraphics; }
    bool touch() const { return m_touch; }
    // True when an interface holds a 192.168.4.x address, i.e. the PC is
    // joined to the camera's softAP. Useful in diagnostics: a viewer that
    // cannot reach the camera usually has not joined it.
    bool onCameraAccessPoint() const { return m_onAp; }

    Q_INVOKABLE void refresh();

signals:
    void entriesChanged();

private:
    void probe();

    QVariantList m_entries;
    bool m_hardwareGraphics = false;
    bool m_touch = false;
    bool m_onAp = false;
};
