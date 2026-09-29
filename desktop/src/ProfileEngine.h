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
        // Where a figure came from, so it can be checked rather than believed.
        // "envelope" and "confirm" are both cells of one JSONL matrix;
        // "d2" is the single Phase 6 baseline run.
        struct Citation
        {
            QString artifact;   // file name under benchmarks/results/
            QString stage;      // envelope | confirm | d2
            QString resolution; // as written in the artifact, e.g. "800x600"
            int quality = 0;
            int fbCount = 0;
            double fps = 0.0;
            double bytes = 0.0; // 0 when the source records no byte figure
        };
        QList<Citation> citations;

        // Mean delivered fps from the Phase 4 envelope (fb3 / latest / psram /
        // xclk 18 MHz, 55 s per cell), plus the Phase 4 130 s confirm stage
        // where one was *admitted*. 0 means no second reading was admitted,
        // which is not the same as "no second reading exists": the confirm
        // stage measured svga/q24 and uxga/q4 too, and both were excluded as
        // below floor (docs/benchmark-results.md, ADR-0008). The test
        // `everyPublishedFigureExistsInTheArtifactItCites` fails if any figure
        // here does not resolve to the configuration it is published against,
        // which is how a VGA number gets kept off an SVGA profile.
        double measuredFps = 0.0;
        double altMeasuredFps = 0.0;
        double medianBytes = 0.0;
        // The rate this profile is held to, and whether the measurement clears
        // it. A profile that does not clear its own floor is still offered -
        // the owner chose quality over frame rate at HD - but it is labelled.
        double floorFps = 15.0;
        bool meetsFloor = false;
        // A floor set by a decision that has not finished being verified.
        // Only HD has one: ADR-0010 scoped it to a provisional >=7 fps and
        // states the >=1 h soak will tighten it. Until that soak runs, a UI
        // that shows 7.0 as a settled number is overstating the record.
        bool floorProvisional = false;
        QString evidence;
    };

    explicit ProfileEngine(QObject *parent = nullptr) : QObject(parent) {}

    // The ladder, best measured image quality first.
    static const QList<Profile> &ladder();
    static const Profile *byId(ProfileId id);

    // What Automatic resolves to with no live measurement to go on: the
    // largest pixel count whose *conservative* reading clears its floor. With
    // the measured envelope that is HD q12, the production default ADR-0010
    // chose - though ADR-0010 scoped HD's floor as provisional, so what the
    // choice rests on is itself still open.
    static const Profile *recommended();

    // The same rule over an explicit ladder. Split out because with the
    // shipped table the conservative rule and the optimistic one agree, so a
    // test over ProfileEngine::ladder() could not tell them apart - only a
    // ladder where a confirm reading dips below its floor can, and building
    // one is the only way to prove recommended() actually consults it.
    // Returns nullptr only for an empty ladder.
    static const Profile *recommendedIn(const QList<Profile> &profiles);

    // The reading a floor test must use: the more conservative of the two
    // measurements where both exist, the single measurement where only one
    // does. Documented in the comment on recommended() for a long time and
    // never actually implemented - the ladder only passed because every alt
    // figure happened to sit at or above its envelope figure.
    static double conservativeFps(const Profile &p);

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
    // Read off the conservative of the two admitted readings (see
    // conservativeFps), not off the envelope alone - the UI shows this.
    Q_PROPERTY(bool activeMeetsFloor READ activeMeetsFloor NOTIFY activeChanged)
    // A floor the UI must not present as settled. Only HD has one: ADR-0010
    // scoped it provisionally and the >=1 h soak has not replaced it yet.
    Q_PROPERTY(bool activeFloorProvisional READ activeFloorProvisional NOTIFY activeChanged)
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
    bool activeFloorProvisional() const;
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
