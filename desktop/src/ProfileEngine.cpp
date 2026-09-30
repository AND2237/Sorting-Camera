#include "ProfileEngine.h"

#include <QJsonObject>

#include <cmath>

namespace {

// Measured, not assumed. Sources for every row are in
// docs/benchmark-results.md:
//   envelope  - Phase 4 matrix, fb3 / latest / psram / xclk 18 MHz, 55 s per cell
//   confirm   - Phase 4 matrix confirm stage, fb2, 130 s per point; both live in
//               benchmarks/results/phase4-20260925-fixed.jsonl
//   d2        - phase6-baseline-20260928-hd-q12-x18.json, one 120 s run
// The confirm stage is part of the Phase 4 harness (benchmark-plan "P4-matrix",
// 11 x 130 s confirms), not Phase 5 - earlier comments here said Phase 5 and
// every citation inherited the mistake.
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
                "phase4 envelope uxga/q4 1.04 fps, 343,758 B; phase4 confirm uxga/q4 "
                "2.16 fps, 241,153 B (fb2) - both far below the 15 fps floor, so the "
                "profile is offered but labelled under floor and Automatic never picks it");
            p.citations = {
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("envelope"),
                 QStringLiteral("1600x1200"), 4, 3, 1.04, 343758.0},
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("confirm"),
                 QStringLiteral("1600x1200"), 4, 2, 2.16, 241153.0},
            };
            l.append(p);
        }

        // High Quality. This is the production default ADR-0010 recorded: the
        // owner chose 1280x720 q12 on 2026-09-27, preferring image quality
        // first, and ADR-0010 decision 1 keeps it as the default. It is also
        // the only ladder entry whose second reading comes from a Phase 6 run
        // rather than from the confirm stage.
        //
        // The floor is 7, not 15, because the owner chose image quality over
        // the frame-rate target at HD and scoping the floor per profile is how
        // that conflict was resolved. It is *provisional*: ADR-0010 says so in
        // as many words and states the >=1 h soak will tighten it to a
        // sustained figure. That soak has not run, so floorProvisional stays
        // set and the UI must not present 7.0 as settled. Until D1 runs, HD
        // also does not meet the >=15 fps §34 target - ADR-0010 decision 3
        // requires publishing that as measured, and we do.
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
            // One 120 s run reported three ways: capture 10.000, delivery
            // 9.966, decoded 9.958 fps - mean 9.9748 -> 9.97. Their agreement
            // is the evidence that the PC keeps up with the camera; it is not
            // a repeatability measurement, and there was only ever one run.
            p.altMeasuredFps = 9.97;
            // Envelope cell for hd/q12, the same cell measuredFps came from.
            // D2 measured the same configuration at 28,956 B per frame - a
            // less detailed scene - and the evidence string says so rather
            // than quietly replacing one with the other.
            p.medianBytes = 57317.0;
            p.floorFps = 7.0;
            p.meetsFloor = true;
            p.floorProvisional = true;
            p.evidence = QStringLiteral(
                "phase4 envelope hd/q12 8.96 fps, 57,317 B; phase6 D2 baseline single 120 s run "
                "9.96/9.97/10.00 fps, 28,956 B (one run measured three ways, not three runs); "
                "7 fps floor is provisional per ADR-0010 pending the 1 h soak");
            p.citations = {
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("envelope"),
                 QStringLiteral("1280x720"), 12, 3, 8.96, 57317.0},
                {QStringLiteral("phase6-baseline-20260928-hd-q12-x18.json"),
                 QStringLiteral("d2"), QStringLiteral("hd"), 12, 3, 9.97, 28956.0},
            };
            l.append(p);
        }

        // Balanced. The largest pixel count that clears the general >=15 floor
        // with margin, and comfortably above the preferred 20.
        //
        // This entry carried a borrowed figure. The confirm stage *did* run
        // svga/q24 - 14.37 fps at 23,077 B over 130 s, below the floor, listed
        // in docs/benchmark-results.md under "measured, excluded from ladder"
        // - so the old comment claiming it did not test this point was false.
        // It is excluded from the ladder by ADR-0008, which is why
        // altMeasuredFps stays 0: that field means "no second reading was
        // admitted", not "no second reading exists".
        //
        // The two figures that used to be here belonged to vga/q24: 18.97 fps
        // (described as "the more conservative reading of the same point" -
        // it is a different resolution) and 14,680 B, which is where the byte
        // count came from. A published figure has to be one the profile's own
        // configuration produced.
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
            p.altMeasuredFps = 0.0;
            p.medianBytes = 16982.0; // envelope cell, the one measuredFps came from
            p.floorFps = 15.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope svga/q24 22.50 fps, 16,982 B; phase4 confirm svga/q24 "
                "14.37 fps, 23,077 B - below the 15 fps floor, excluded from the confirm "
                "ladder in benchmark-results.md (ADR-0008)");
            p.citations = {
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("envelope"),
                 QStringLiteral("800x600"), 24, 3, 22.50, 16982.0},
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("confirm"),
                 QStringLiteral("800x600"), 24, 2, 14.37, 23077.0},
            };
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
            p.medianBytes = 8086.0; // confirm cell for qvga/q12, cited below
            p.floorFps = 20.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope qvga/q12 44.95 fps, 7,119 B; phase4 confirm qvga/q12 "
                "42.00 fps, 8,086 B");
            p.citations = {
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("envelope"),
                 QStringLiteral("320x240"), 12, 3, 44.95, 7119.0},
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("confirm"),
                 QStringLiteral("320x240"), 12, 2, 42.00, 8086.0},
            };
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
            p.medianBytes = 4069.0; // confirm cell for qvga/q36, cited below
            p.floorFps = 20.0;
            p.meetsFloor = true;
            p.evidence = QStringLiteral(
                "phase4 envelope qvga/q36 45.01 fps, 4,066 B; phase4 confirm qvga/q36 "
                "44.93 fps, 4,069 B");
            p.citations = {
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("envelope"),
                 QStringLiteral("320x240"), 36, 3, 45.01, 4066.0},
                {QStringLiteral("phase4-20260925-fixed.jsonl"), QStringLiteral("confirm"),
                 QStringLiteral("320x240"), 36, 2, 44.93, 4069.0},
            };
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

double ProfileEngine::conservativeFps(const Profile &p)
{
    // Two readings exist: take the lower one, because the question is what
    // this profile can be relied on to deliver, not what it managed once.
    // Only one exists: that one is the answer. altMeasuredFps == 0 means "no
    // second reading was admitted", never "a second reading of zero fps" -
    // treating it as the latter would push Balanced, High FPS and Low Latency
    // all under their floors on the strength of a missing number.
    if (p.altMeasuredFps > 0.0 && p.measuredFps > 0.0) {
        return qMin(p.altMeasuredFps, p.measuredFps);
    }
    return p.measuredFps;
}

const ProfileEngine::Profile *ProfileEngine::recommendedIn(const QList<Profile> &profiles)
{
    if (profiles.isEmpty()) {
        return nullptr;
    }
    const Profile *best = nullptr;
    for (const Profile &p : profiles) {
        if (conservativeFps(p) >= p.floorFps) {
            if (!best || p.pixels > best->pixels
                || (p.pixels == best->pixels && p.quality < best->quality)) {
                best = &p;
            }
        }
    }
    return best ? best : &profiles.first();
}

const ProfileEngine::Profile *ProfileEngine::recommended()
{
    return recommendedIn(ladder());
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
    // Judged on the conservative reading, deliberately: this is the flag
    // Main.qml colours red with and CameraDevice.cpp prints as "UNDER FLOOR",
    // so letting it consult only the optimistic envelope reading would report
    // the higher number the moment a second reading lands below it. The
    // Profile::meetsFloor field stays the envelope-based fact it was.
    if (!m_activeProfile) {
        return false;
    }
    return conservativeFps(*m_activeProfile) >= m_activeProfile->floorFps;
}

bool ProfileEngine::activeFloorProvisional() const
{
    return m_activeProfile && m_activeProfile->floorProvisional;
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

    // A custom configuration has no measured operating point to speak about,
    // and the sentence below reads m_activeProfile - so the guard belongs
    // before it, not after it (audit section 17, group E: the shortfall branch
    // could dereference a null profile).
    if (!m_activeProfile) {
        return QStringLiteral("Custom configuration - no measured operating point matches it");
    }

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

    if (m_activeProfile->measuredFps < m_activeProfile->floorFps) {
        return QStringLiteral("%1 measured %2 fps, under its %3 fps floor")
            .arg(m_activeProfile->name)
            .arg(m_activeProfile->measuredFps, 0, 'f', 2)
            .arg(m_activeProfile->floorFps, 0, 'f', 0);
    }
    return QString();
}
