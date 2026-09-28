#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// Master Prompt 12, adaptive performance profiles, and section 26's "adaptive
// quality / profile mode".
//
// Every number in the ladder below was measured on this hardware and is cited
// in docs/benchmark-results.md. Nothing here is an estimate of what a
// different sensor or a different scene would do, and the per-point values
// carry the scene dependence that document already warns about: the same
// svga/q12 point measured 19.65, 15.91 and 9.95 fps in one session. The
// ordering held; the absolute values did not. That is why the floors are
// carried per profile instead of a single global number.
class ProfileEngine : public QObject
{
    Q_OBJECT

public:
    enum class ProfileId {
        MaximumQuality,
        HighQuality,
        Balanced,
        HighFps,
        LowLatency,
        Custom,
    };
    Q_ENUM(ProfileId)

    // One operating point, and what was measured about it.
    struct Profile
    {
        ProfileId id = ProfileId::Custom;
        QString name;
        QString framesize;
        int quality = 12;
        int xclkMhz = 18;
        int frameBufferCount = 3;
        QString grabMode = QStringLiteral("latest");
        double pixels = 0.0;
        // Mean delivered fps from the Phase 4 envelope (fb3 / latest / psram /
        // xclk 18 MHz, 55 s per cell), plus a second, longer measurement where
        // one exists. 0 means no second run covered this exact point, and the
        // evidence string says so rather than borrowing a neighbouring one -
        // the 130 s confirm ladder did not test every resolution and quality
        // combination, and putting a VGA number on an SVGA profile would be a
        // fabricated data point.
        double measuredFps = 0.0;
        double altMeasuredFps = 0.0;
        double medianBytes = 0.0;
        // The rate this profile is held to, and whether the measurement clears
        // it. A profile that does not clear its own floor is still offered -
        // the owner chose quality over frame rate at HD - but it is labelled.
        double floorFps = 15.0;
        bool meetsFloor = false;
        QString evidence;
    };

    explicit ProfileEngine(QObject *parent = nullptr) : QObject(parent) {}

    // The ladder, best measured image quality first.
    static const QList<Profile> &ladder();
    static const Profile *byId(ProfileId id);

    // What Automatic resolves to with no live measurement to go on: the
    // largest pixel count whose measurement clears its floor. With the
    // measured envelope that is HD q12, which is the production default
    // ADR-0010 settled on.
    static const Profile *recommended();

    // Which profile a hand-set configuration corresponds to, or Custom when it
    // matches none. Custom is a real answer, not a failure: the user is
    // entitled to an operating point the ladder does not describe.
    static ProfileId matchConfig(const QString &framesize, int quality, int xclkMhz,
                                 int frameBufferCount, const QString &grabMode);

    // What the ladder says about a configuration that matches no profile:
    // the closest point by resolution, for the advisory text.
    static const Profile *closestByResolution(const QString &framesize);

    // The camera reports a resolution as "1280x720" and everything else in
    // this class speaks framesize keys, so the mapping lives with the profile
    // table it has to stay consistent with.
    static QString framesizeKeyFromResolution(const QString &resolution);
    static QString grabModeFromFirmware(const QString &grabMode);

    Q_PROPERTY(int mode READ modeIndex WRITE setModeIndex NOTIFY modeChanged)
    Q_PROPERTY(QStringList names READ profileNames CONSTANT)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeChanged)
    Q_PROPERTY(QString activeName READ activeName NOTIFY activeChanged)
    Q_PROPERTY(bool activeIsCustom READ activeIsCustom NOTIFY activeChanged)
    Q_PROPERTY(double activeMeasuredFps READ activeMeasuredFps NOTIFY activeChanged)
    Q_PROPERTY(double activeFloorFps READ activeFloorFps NOTIFY activeChanged)
    Q_PROPERTY(bool activeMeetsFloor READ activeMeetsFloor NOTIFY activeChanged)
    Q_PROPERTY(QString activeEvidence READ activeEvidence NOTIFY activeChanged)
    // The profile Automatic would move to, given what has been observed so
    // far, and why. Empty advice means the current profile is the right one.
    Q_PROPERTY(QString advice READ advice NOTIFY adviceChanged)
    Q_PROPERTY(double observedFps READ observedFps NOTIFY adviceChanged)

    // 0 = Automatic, 1..N = the ladder in order.
    QStringList profileNames() const;

    // The ladder's settings, exposed so the UI applies what the table says
    // instead of keeping its own copy. Duplicating these in QML is how a
    // profile ends up labelled one thing and configured as another.
    Q_INVOKABLE QString framesizeAt(int index) const;
    Q_INVOKABLE int qualityAt(int index) const;
    Q_INVOKABLE int xclkAt(int index) const;
    Q_INVOKABLE int frameBufferCountAt(int index) const;
    Q_INVOKABLE QString grabModeAt(int index) const;
    Q_INVOKABLE double measuredFpsAt(int index) const;

    // The profile Automatic resolves to, as a 1-based ladder index (0 when
    // there is none). Used to actually apply the policy, not just describe it.
    Q_INVOKABLE int recommendedIndex() const;
    Q_INVOKABLE QString recommendedName() const;
    int modeIndex() const;
    void setModeIndex(int index);

    // Told about the camera's current configuration whenever it changes.
    void setConfig(const QString &framesize, int quality, int xclkMhz, int frameBufferCount,
                   const QString &grabMode);

    int activeIndex() const;
    QString activeName() const;
    bool activeIsCustom() const;
    double activeMeasuredFps() const;
    double activeFloorFps() const;
    bool activeMeetsFloor() const;
    QString activeEvidence() const;
    QString advice() const;
    double observedFps() const;

    // Feeds the live frame rate. Sustained shortfall against the active
    // profile's floor is what makes Automatic offer a step down; a single slow
    // window does not, because the measurements above show a fixed point moving
    // by a factor of two with the scene alone.
    void observeFps(double fps);
    void clearObservation();

    // Number of consecutive shortfall windows before advice appears. Exposed
    // so the behaviour is pinned by a test rather than by a magic number in a
    // timer callback.
    static int shortfallWindowsRequired();

signals:
    void modeChanged();
    void activeChanged();
    void adviceChanged();

private:
    int m_modeIndex = 0;
    ProfileId m_active = ProfileId::HighQuality;
    const Profile *m_activeProfile = nullptr;
    QString m_configFramesize;
    int m_configQuality = 0;
    int m_configXclk = 0;
    int m_configFb = 0;
    QString m_configGrab;
    double m_observedFps = 0.0;
    int m_shortWindows = 0;
};
