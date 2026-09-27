#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

class QMutex;

class AppMetrics
{
public:
    static AppMetrics &instance();

    void reset();

    void addDecodeUs(qint64 us);
    void addParseUs(qint64 us);
    void addPresentAgeMs(qint64 ms);
    void addRenderAgeMs(qint64 ms);
    void addRenderUs(qint64 us);
    void addScaleUs(qint64 us);
    void addReceiveGapMs(qint64 ms);

    void countBytes(qint64 bytes);
    void countPart();
    void countDecoded();
    void countDecodeFailed();
    void countOverwritten();
    void countStaleDropped();
    void countPresented();

    qint64 uptimeMs() const;
    double processCpuPercent() const;
    qint64 workingSetBytes() const;

    QJsonObject snapshot() const;
    bool writeJson(const QString &path, const QJsonObject &extra) const;

    static qint64 nowMs();
    static qint64 nowUs();

private:
    AppMetrics();
    ~AppMetrics();

    AppMetrics(const AppMetrics &) = delete;
    AppMetrics &operator=(const AppMetrics &) = delete;

    struct Series
    {
        QList<qint64> samples;
        qint64 dropped = 0;
        qint64 total = 0;
    };

    void add(Series &series, qint64 value);
    QJsonObject seriesJson(const Series &series) const;

    mutable QMutex *m_mutex = nullptr;
    Series m_decode;
    Series m_parse;
    Series m_presentAge;
    Series m_renderAge;
    Series m_render;
    Series m_scale;
    Series m_receiveGap;
    qint64 m_bytes = 0;
    qint64 m_parts = 0;
    qint64 m_decoded = 0;
    qint64 m_decodeFailed = 0;
    qint64 m_overwritten = 0;
    qint64 m_staleDropped = 0;
    qint64 m_presented = 0;
    qint64 m_startMs = 0;
};
