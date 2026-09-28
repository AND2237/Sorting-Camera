#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class FrameBus;

// Master Prompt 22. Saves the exact JPEG bytes received from the camera: no
// decode, no re-encode, no resize, no quality change. The file on disk is the
// network payload.
//
// The bytes are pulled from FrameBus rather than passed in from QML, so the
// frame payload never crosses into the scripting layer to be copied around.
class SnapshotWriter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastPath READ lastPath NOTIFY lastSavedChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)

public:
    explicit SnapshotWriter(FrameBus *bus, QObject *parent = nullptr);

    QString lastPath() const;
    QString errorString() const;
    bool isAvailable() const;

    // Saves whatever frame is currently held. Returns the path written, or an
    // empty string on failure (see errorString).
    Q_INVOKABLE QString save(const QString &directory, const QString &deviceId);

    static QString suggestedFileName(const QString &deviceId);

signals:
    void lastSavedChanged();
    void errorStringChanged();
    void availableChanged();
    void saved(const QString &path);

private:
    void setError(const QString &err);

    FrameBus *m_bus = nullptr;
    QString m_lastPath;
    QString m_error;
};
