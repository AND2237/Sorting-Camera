#include "ProfileEngine.h"

#include <QJsonObject>

#include <cmath>

namespace {

// Measured, not assumed. Sources for every row are in
// docs/benchmark-results.md:
//   envelope  - Phase 4, fb3 / latest / psram / xclk 18 MHz, 55 s per cell
//   confirm   - Phase 5 confirm ladder, fb2, 130 s per point
//   d2        - phase6-baseline-20260928-hd-q12-x18.json, three runs
// Pixels are width*height in megapixels and are what "better image" is ranked
// on; at equal pixels the higher JPEG quality wins, which is the second key.
const QList<ProfileEngine::Profile> &buildLadder()
{
    using P = ProfileEngine::Profile;
    static const QList<P> ladder = [] {
        QList<P> l;

        // Maximum Quality. UXGA q4 is the largest, least compressed frame the
        // sensor produces: 1.92 MP. It is offered because it is a real
        // operating point, and it is held to the general >=15 fps floor like
        // everything except HD - and it fails that floor, by a wide margin.
        // 1.04 fps is what it actually delivered; the 130 s confirm run
        // measured 2.16 fps, so even the more generous reading is nowhere near
        // the target. It is labelled as under floor in the UI and Automatic
        // will not select it, which is the honest outcome: the owner chose
        // quality over frame rate at 0.92 MP, not at any cost.
        {
            P p;
            p.id = ProfileEngine::ProfileId::MaximumQuality;
            p.name = QStringLiteral("Maximum Quality");
            p.framesize = QStringLiteral("uxga");
            p.quality = 4;
            p.xclkMhz = 18;
            p.frameBufferCount = 3;
            p.grabMode = QStringLiteral("latest");
            p.pixels = 1600.0 * 1200.0 / 1e6;
            p.measuredFps = 1.04;
            p.altMeasuredFps = 2.16;
            p.medianBytes = 0.0;
            p.floorFps = 15.0;
            p.meetsFloor = false;
            p.evidence = QStringLiteral(
                "phase4 envelope uxga/q4 1.04 fps; phase5 confirm uxga/q4 2.16 fps (fb2)");
            l.append(p);
        }

        // High Quality. This is the production default ADR-0010 settled on:
        // 1280x720 q12, and the only ladder entry whose own measurement sits
        // in the D2 baseline rather than only in the envelope. Its floor is 7,
        // not 15, because the owner chose image quality over the frame-rate
        // target at HD and scoping the floor per profile is how that conflict
        // was resolved.
        {
            P p;
            p.id = ProfileEngine::ProfileId::HighQuality;
            p.name = QStringLiteral("High Quality");
            p.framesize = QStringLiteral("hd");
            p.quality = 12;
            p.xclkMhz = 18;
            p.frameBufferCount = 3;
            p.grabMode = QStringLiteral("latest");
            p.pixels = 1280.0 * 720.0 / 1e6;
            p.measuredFps = 8.96;
            p.altMeasuredFps = 9.98; // mean of the three D2 runs 10.00/9.97/9.96
            p.medianBytes = 58000.0;
            p.floorFps = 7.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope hd/q12 8.96 fps; phase6 D2 baseline 10.00/9.97/9.96 fps, 58 KB");
            l.append(p);
        }

        // Balanced. The largest pixel count that clears the general >=15 floor
        // with margin, and comfortably above the preferred 20. The confirm run
        // at 18.97 fps is the more conservative reading of the same point.
        {
            P p;
            p.id = ProfileEngine::ProfileId::Balanced;
            p.name = QStringLiteral("Balanced");
            p.framesize = QStringLiteral("svga");
            p.quality = 24;
            p.xclkMhz = 18;
            p.frameBufferCount = 3;
            p.grabMode = QStringLiteral("latest");
            p.pixels = 800.0 * 600.0 / 1e6;
            p.measuredFps = 22.50;
            p.altMeasuredFps = 0.0; // confirm ladder did not test svga/q24
            p.medianBytes = 14680.0;
            p.floorFps = 15.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope svga/q24 22.50 fps; the 130 s confirm ladder tested svga/q36 (16.34) and vga/q24 (18.97) but not this exact point");
            l.append(p);
        }

        // High FPS. The fastest measured point that still carries a useful
        // frame: 45 fps, 8 KB median. Quality drops to q12 here rather than
        // lower because q4 at QVGA hit the driver's frame-buffer cap on a
        // detailed scene and collapsed to 0.51 fps before the watch dog rebooted
        // the camera. That is a measured reason not to offer the fastest
        // possible quality setting at this resolution.
        {
            P p;
            p.id = ProfileEngine::ProfileId::HighFps;
            p.name = QStringLiteral("High FPS");
            p.framesize = QStringLiteral("qvga");
            p.quality = 12;
            p.xclkMhz = 18;
            p.frameBufferCount = 3;
            p.grabMode = QStringLiteral("latest");
            p.pixels = 320.0 * 240.0 / 1e6;
            p.measuredFps = 44.95;
            p.altMeasuredFps = 42.00;
            p.medianBytes = 8086.0;
            p.floorFps = 20.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope qvga/q12 44.95 fps; phase5 confirm qvga/q12 42.00 fps, 8.1 KB");
            l.append(p);
        }

        // Low Latency. Same resolution as High FPS on purpose. At this sensor
        // the frame interval is set by resolution, so with 45 fps already
        // available the only remaining lever on how old the picture is is how
        // much data each frame costs to move and decode. q36 measured 4069 B
        // median against q12's 8086 B, which is the difference the profile is
        // actually buying.
        {
            P p;
            p.id = ProfileEngine::ProfileId::LowLatency;
            p.name = QStringLiteral("Low Latency");
            p.framesize = QStringLiteral("qvga");
            p.quality = 36;
            p.xclkMhz = 18;
            p.frameBufferCount = 3;
            p.grabMode = QStringLiteral("latest");
            p.pixels = 320.0 * 240.0 / 1e6;
            p.measuredFps = 45.01;
            p.altMeasuredFps = 44.93;
            p.medianBytes = 4069.0;
            p.floorFps = 20.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope qvga/q36 45.01 fps; phase5 confirm qvga/q36 44.93 fps, 4069 B");
            l.append(p);
        }

        return l;
    }();
    return ladder;
}

} // namespace

const QList<ProfileEngine::Profile> &ProfileEngine::ladder()
{
    return buildLadder();
}

const ProfileEngine::Profile *ProfileEngine::byId(ProfileId id)
{
    for (const Profile &p : ladder()) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const ProfileEngine::Profile *ProfileEngine::recommended()
{
    // Project priority is image quality > FPS > latency, so the automatic
    // choice is the largest pixel count that clears its own floor, taking the
    // more conservative of the two measurements when both exist. Scanning the
    // ladder rather than hard-coding an answer is what keeps the rule and the
    // data separable: if a better HD measurement ever appeared, this would
    // still be the same rule that picked it.
    const Profile *best = nullptr;
    for (const Profile &p : ladder()) {
        if (p.meetsFloor && p.measuredFps >= p.floorFps) {
            if (!best || p.pixels > best->pixels
                || (p.pixels == best->pixels && p.quality < best->quality)) {
                best = &p;
            }
        }
    }
    return best ? best : &ladder().first();
}

ProfileEngine::ProfileId ProfileEngine::matchConfig(const QString &framesize, int quality, int xclkMhz,
                                                    int frameBufferCount, const QString &grabMode)
{
    for (const Profile &p : ladder()) {
        if (p.framesize == framesize && p.quality == quality && p.xclkMhz == xclkMhz
            && p.frameBufferCount == frameBufferCount && p.grabMode == grabMode) {
            return p.id;
        }
    }
    return ProfileId::Custom;
}

const ProfileEngine::Profile *ProfileEngine::closestByResolution(const QString &framesize)
{
    // Only used for advisory text, so a nearest-pixel-count match is enough.
    static const struct {
        const char *key;
        double pixels;
    } sizes[] = {
        {"qqvga", 160.0 * 120.0 / 1e6}, {"qvga", 320.0 * 240.0 / 1e6},
        {"vga", 640.0 * 480.0 / 1e6},   {"svga", 800.0 * 600.0 / 1e6},
        {"xga", 1024.0 * 768.0 / 1e6},  {"hd", 1280.0 * 720.0 / 1e6},
        {"sxga", 1280.0 * 1024.0 / 1e6},{"uxga", 1600.0 * 1200.0 / 1e6},
    };
    double want = 0.0;
    for (const auto &s : sizes) {
        if (framesize == QLatin1String(s.key)) {
            want = s.pixels;
            break;
        }
    }
    if (want <= 0.0) {
        return nullptr;
    }
    const Profile *best = nullptr;
    double bestDelta = 0.0;
    for (const Profile &p : ladder()) {
        const double delta = std::fabs(p.pixels - want);
        if (!best || delta < bestDelta) {
            best = &p;
            bestDelta = delta;
        }
    }
    return best;
}

QStringList ProfileEngine::profileNames() const
{
    QStringList names;
    for (const Profile &p : ladder()) {
        names << p.name;
    }
    return names;
}

QString ProfileEngine::framesizeKeyFromResolution(const QString &resolution)
{
    static const struct {
        const char *dims;
        const char *key;
    } table[] = {
        {"160x120", "qqvga"},  {"320x240", "qvga"},  {"640x480", "vga"},
        {"800x600", "svga"},   {"1024x768", "xga"},  {"1280x720", "hd"},
        {"1280x1024", "sxga"}, {"1600x1200", "uxga"},
    };
    for (const auto &row : table) {
        if (resolution == QLatin1String(row.dims)) {
            return QString::fromLatin1(row.key);
        }
    }
    return QString();
}

QString ProfileEngine::grabModeFromFirmware(const QString &grabMode)
{
    // The firmware names the non-latest mode "when_empty"; the desktop and the
    // profile table call it "cont". Left here so the two vocabularies meet in
    // one place instead of at each comparison.
    if (grabMode == QStringLiteral("when_empty")) {
        return QStringLiteral("cont");
    }
    return grabMode;
}

namespace {
const ProfileEngine::Profile *atIndex(int index)
{
    const QList<ProfileEngine::Profile> &l = ProfileEngine::ladder();
    if (index <= 0 || index > l.size()) {
        return nullptr;
    }
    return &l.at(index - 1);
}
} // namespace

QString ProfileEngine::framesizeAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->framesize : QString();
}

int ProfileEngine::qualityAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->quality : 0;
}

int ProfileEngine::xclkAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->xclkMhz : 0;
}

int ProfileEngine::frameBufferCountAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->frameBufferCount : 0;
}

QString ProfileEngine::grabModeAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->grabMode : QString();
}

double ProfileEngine::measuredFpsAt(int index) const
{
    const Profile *p = atIndex(index);
    return p ? p->measuredFps : 0.0;
}

int ProfileEngine::recommendedIndex() const
{
    const Profile *rec = recommended();
    if (!rec) {
        return 0;
    }
    for (int i = 0; i < ladder().size(); ++i) {
        if (ladder().at(i).id == rec->id) {
            return i + 1;
        }
    }
    return 0;
}

QString ProfileEngine::recommendedName() const
{
    const Profile *rec = recommended();
    return rec ? rec->name : QString();
}

int ProfileEngine::modeIndex() const
{
    return m_modeIndex;
}

void ProfileEngine::setModeIndex(int index)
{
    // 0 = Automatic, 1..N = the ladder. Anything else would come from a
    // selector that is out of step with the table, and every read of the
    // ladder by index assumes this range holds.
    const int clamped = qBound(0, index, ladder().size());
    if (m_modeIndex == clamped) {
        return;
    }
    m_modeIndex = clamped;
    emit modeChanged();
    emit adviceChanged();
}

void ProfileEngine::setConfig(const QString &framesize, int quality, int xclkMhz,
                              int frameBufferCount, const QString &grabMode)
{
    if (framesize.isEmpty()) {
        return;
    }
    if (m_configFramesize == framesize && m_configQuality == quality && m_configXclk == xclkMhz
        && m_configFb == frameBufferCount && m_configGrab == grabMode) {
        return;
    }
    m_configFramesize = framesize;
    m_configQuality = quality;
    m_configXclk = xclkMhz;
    m_configFb = frameBufferCount;
    m_configGrab = grabMode;

    const ProfileId id = matchConfig(framesize, quality, xclkMhz, frameBufferCount, grabMode);
    const Profile *p = byId(id);
    if (m_active != id || m_activeProfile != p) {
        m_active = id;
        m_activeProfile = p;
        // A new operating point is a new question; the old evidence about the
        // previous one does not carry over.
        m_shortWindows = 0;
        emit activeChanged();
    }
    emit adviceChanged();
}

int ProfileEngine::activeIndex() const
{
    for (int i = 0; i < ladder().size(); ++i) {
        if (ladder().at(i).id == m_active) {
            return i + 1;
        }
    }
    return ladder().size() + 1;
}

QString ProfileEngine::activeName() const
{
    return m_activeProfile ? m_activeProfile->name : QStringLiteral("Custom");
}

bool ProfileEngine::activeIsCustom() const
{
    return m_active == ProfileId::Custom;
}

double ProfileEngine::activeMeasuredFps() const
{
    return m_activeProfile ? m_activeProfile->measuredFps : 0.0;
}

double ProfileEngine::activeFloorFps() const
{
    return m_activeProfile ? m_activeProfile->floorFps : 0.0;
}

bool ProfileEngine::activeMeetsFloor() const
{
    return m_activeProfile && m_activeProfile->measuredFps >= m_activeProfile->floorFps;
}

QString ProfileEngine::activeEvidence() const
{
    return m_activeProfile ? m_activeProfile->evidence : QString();
}

double ProfileEngine::observedFps() const
{
    return m_observedFps;
}

int ProfileEngine::shortfallWindowsRequired()
{
    // Three consecutive windows. The measurements in docs/benchmark-results.md
    // show the same operating point moving between 9.95 and 19.65 fps purely
    // with the scene, so a single slow window is evidence of nothing.
    return 3;
}

void ProfileEngine::observeFps(double fps)
{
    if (fps < 0.0) {
        return;
    }
    m_observedFps = fps;
    if (m_activeProfile && m_activeProfile->floorFps > 0.0) {
        // Ten percent of slack before a window counts as a shortfall, so a
        // profile sitting exactly on its floor is not told to move.
        if (fps < m_activeProfile->floorFps * 0.9) {
            ++m_shortWindows;
        } else {
            m_shortWindows = 0;
        }
    } else {
        m_shortWindows = 0;
    }
    emit adviceChanged();
}

void ProfileEngine::clearObservation()
{
    m_observedFps = 0.0;
    m_shortWindows = 0;
    emit adviceChanged();
}

QString ProfileEngine::advice() const
{
    if (m_modeIndex != 0) {
        // Manual: the user picked. Saying anything would be second-guessing a
        // deliberate choice on every status refresh.
        return QString();
    }
    if (m_activeProfile && m_shortWindows < shortfallWindowsRequired()) {
        return QString();
    }

    // Automatic. The floor the current point is held to, and the next rung down
    // when one is warranted.
    const QList<Profile> &l = ladder();
    int here = activeIndex() - 1;
    for (int i = 0; i < l.size(); ++i) {
        if (l.at(i).id == m_active) {
            here = i;
            break;
        }
    }
    if (here < 0 || here + 1 >= l.size()) {
        return QString();
    }
    const Profile &next = l.at(here + 1);

    if (m_shortWindows >= shortfallWindowsRequired() && m_observedFps > 0.0) {
        return QStringLiteral("measured %1 fps against a %2 fps floor for %3 consecutive "
                              "windows - %4 (%5 fps measured) trades %6 MP for %7 fps")
            .arg(m_observedFps, 0, 'f', 1)
            .arg(m_activeProfile->floorFps, 0, 'f', 0)
            .arg(m_shortWindows)
            .arg(next.name)
            .arg(next.measuredFps, 0, 'f', 1)
            .arg(m_activeProfile->pixels, 0, 'f', 2)
            .arg(next.measuredFps, 0, 'f', 1);
    }

    if (!m_activeProfile) {
        return QStringLiteral("Custom configuration - no measured operating point matches it");
    }
    if (m_activeProfile->measuredFps < m_activeProfile->floorFps) {
        return QStringLiteral("%1 measured %2 fps, under its %3 fps floor")
            .arg(m_activeProfile->name)
            .arg(m_activeProfile->measuredFps, 0, 'f', 2)
            .arg(m_activeProfile->floorFps, 0, 'f', 0);
    }
    return QString();
}
