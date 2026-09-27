#include "AppMetrics.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMutex>
#include <QMutexLocker>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
constexpr int kMaxSamplesPerSeries = 200000;

QElapsedTimer &clockRef()
{
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return timer;
}

qint64 percentile(const QList<qint64> &sorted, double p)
{
    if (sorted.isEmpty()) {
        return 0;
    }
    int idx = int(p * double(sorted.size() - 1) + 0.5);
    idx = qBound(0, idx, sorted.size() - 1);
    return sorted.at(idx);
}
} // namespace

AppMetrics::AppMetrics()
    : m_mutex(new QMutex)
{
    clockRef();
    m_startMs = nowMs();
}

AppMetrics::~AppMetrics()
{
    delete m_mutex;
}

AppMetrics &AppMetrics::instance()
{
    static AppMetrics metrics;
    return metrics;
}

qint64 AppMetrics::nowMs()
{
    return clockRef().elapsed();
}

qint64 AppMetrics::nowUs()
{
    return clockRef().nsecsElapsed() / 1000;
}

void AppMetrics::reset()
{
    QMutexLocker lock(m_mutex);
    m_decode = Series();
    m_parse = Series();
    m_presentAge = Series();
    m_renderAge = Series();
    m_render = Series();
    m_scale = Series();
    m_receiveGap = Series();
    m_bytes = 0;
    m_parts = 0;
    m_decoded = 0;
    m_decodeFailed = 0;
    m_overwritten = 0;
    m_staleDropped = 0;
    m_presented = 0;
    m_startMs = nowMs();
}

void AppMetrics::add(Series &series, qint64 value)
{
    if (series.samples.size() < kMaxSamplesPerSeries) {
        series.samples.append(value);
    } else {
        ++series.dropped;
    }
    ++series.total;
}

void AppMetrics::addDecodeUs(qint64 us) { QMutexLocker l(m_mutex); add(m_decode, us); }
void AppMetrics::addParseUs(qint64 us) { QMutexLocker l(m_mutex); add(m_parse, us); }
void AppMetrics::addPresentAgeMs(qint64 ms) { QMutexLocker l(m_mutex); add(m_presentAge, ms); }
void AppMetrics::addRenderAgeMs(qint64 ms) { QMutexLocker l(m_mutex); add(m_renderAge, ms); }
void AppMetrics::addRenderUs(qint64 us) { QMutexLocker l(m_mutex); add(m_render, us); }
void AppMetrics::addScaleUs(qint64 us) { QMutexLocker l(m_mutex); add(m_scale, us); }
void AppMetrics::addReceiveGapMs(qint64 ms) { QMutexLocker l(m_mutex); add(m_receiveGap, ms); }

void AppMetrics::countBytes(qint64 bytes)
{
    QMutexLocker l(m_mutex);
    m_bytes += bytes;
}

void AppMetrics::countPart()
{
    QMutexLocker l(m_mutex);
    ++m_parts;
}

void AppMetrics::countDecoded()
{
    QMutexLocker l(m_mutex);
    ++m_decoded;
}

void AppMetrics::countDecodeFailed()
{
    QMutexLocker l(m_mutex);
    ++m_decodeFailed;
}

void AppMetrics::countOverwritten()
{
    QMutexLocker l(m_mutex);
    ++m_overwritten;
}

void AppMetrics::countStaleDropped()
{
    QMutexLocker l(m_mutex);
    ++m_staleDropped;
}

void AppMetrics::countPresented()
{
    QMutexLocker l(m_mutex);
    ++m_presented;
}

qint64 AppMetrics::uptimeMs() const
{
    QMutexLocker l(m_mutex);
    return nowMs() - m_startMs;
}

double AppMetrics::processCpuPercent() const
{
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return -1.0;
    }
    const quint64 k100ns = 10000;
    const double cpuMs = (double(kernel.dwHighDateTime) * 4294967296.0 + kernel.dwLowDateTime) / k100ns
                       + (double(user.dwHighDateTime) * 4294967296.0 + user.dwLowDateTime) / k100ns;
    const qint64 wall = uptimeMs();
    if (wall <= 0) {
        return -1.0;
    }
    return 100.0 * cpuMs / double(wall);
#else
    return -1.0;
#endif
}

qint64 AppMetrics::workingSetBytes() const
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return qint64(pmc.WorkingSetSize);
    }
#endif
    return -1;
}

QJsonObject AppMetrics::seriesJson(const Series &series) const
{
    QJsonObject obj;
    obj["count"] = qint64(series.total);
    obj["dropped_samples"] = series.dropped;
    if (series.samples.isEmpty()) {
        return obj;
    }
    QList<qint64> sorted = series.samples;
    std::sort(sorted.begin(), sorted.end());
    qint64 sum = 0;
    for (qint64 v : sorted) {
        sum += v;
    }
    obj["p50"] = percentile(sorted, 0.50);
    obj["p95"] = percentile(sorted, 0.95);
    obj["p99"] = percentile(sorted, 0.99);
    obj["min"] = sorted.first();
    obj["max"] = sorted.last();
    obj["mean"] = sum / sorted.size();
    return obj;
}

QJsonObject AppMetrics::snapshot() const
{
    QMutexLocker l(m_mutex);

    QJsonObject root;
    root["uptime_ms"] = nowMs() - m_startMs;
    root["bytes_received"] = m_bytes;
    root["parts_parsed"] = m_parts;
    root["frames_decoded"] = m_decoded;
    root["decode_failures"] = m_decodeFailed;
    root["frames_overwritten"] = m_overwritten;
    root["stale_dropped"] = m_staleDropped;
    root["frames_presented"] = m_presented;

    const qint64 wall = qint64(nowMs() - m_startMs);
    if (wall > 0) {
        root["parts_fps"] = 1000.0 * double(m_parts) / double(wall);
        root["decoded_fps"] = 1000.0 * double(m_decoded) / double(wall);
        root["presented_fps"] = 1000.0 * double(m_presented) / double(wall);
        root["receive_mbps"] = (8.0 * double(m_bytes)) / double(wall) / 1000.0;
    }

    root["decode_us"] = seriesJson(m_decode);
    root["parse_us"] = seriesJson(m_parse);
    root["present_age_ms"] = seriesJson(m_presentAge);
    root["render_age_ms"] = seriesJson(m_renderAge);
    root["render_us"] = seriesJson(m_render);
    root["scale_us"] = seriesJson(m_scale);
    root["receive_gap_ms"] = seriesJson(m_receiveGap);
    return root;
}

bool AppMetrics::writeJson(const QString &path, const QJsonObject &extra) const
{
    QJsonObject root = snapshot();
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        root.insert(it.key(), it.value());
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}
