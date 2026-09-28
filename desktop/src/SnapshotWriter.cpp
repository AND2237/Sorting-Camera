#include "SnapshotWriter.h"

#include "FrameBus.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

SnapshotWriter::SnapshotWriter(FrameBus *bus, QObject *parent)
    : QObject(parent), m_bus(bus)
{
    if (m_bus) {
        connect(m_bus, &FrameBus::versionChanged, this, &SnapshotWriter::availableChanged);
    }
}

QString SnapshotWriter::lastPath() const
{
    return m_lastPath;
}

QString SnapshotWriter::errorString() const
{
    return m_error;
}

bool SnapshotWriter::isAvailable() const
{
    return m_bus && m_bus->hasRawFrame();
}

void SnapshotWriter::setError(const QString &err)
{
    if (m_error != err) {
        m_error = err;
        emit errorStringChanged();
    }
}

QString SnapshotWriter::suggestedFileName(const QString &deviceId)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    const QString suffix = deviceId.isEmpty() ? QString() : QStringLiteral("_%1").arg(deviceId);
    return QStringLiteral("snap_%1%2.jpg").arg(stamp, suffix);
}

QString SnapshotWriter::save(const QString &directory, const QString &deviceId)
{
    const QByteArray jpeg = m_bus ? m_bus->rawFrame() : QByteArray();
    if (jpeg.isEmpty()) {
        setError(QStringLiteral("no frame has been received yet"));
        return QString();
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        setError(QStringLiteral("cannot create directory %1").arg(directory));
        return QString();
    }

    const QString path = dir.filePath(suggestedFileName(deviceId));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(QStringLiteral("cannot open %1: %2").arg(path, out.errorString()));
        return QString();
    }

    const qint64 written = out.write(jpeg);
    out.flush();
    if (written != jpeg.size()) {
        setError(QStringLiteral("short write to %1: %2").arg(path, out.errorString()));
        out.close();
        out.remove();
        return QString();
    }
    out.close();

    if (QFileInfo(path).size() != jpeg.size()) {
        setError(QStringLiteral("%1 does not match the received size").arg(path));
        return QString();
    }

    m_lastPath = path;
    setError(QString());
    emit lastSavedChanged();
    emit saved(path);
    return path;
}
