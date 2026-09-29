#pragma once

#include <QVector>

// The order in which a bench run writes the camera settings it was asked for.
//
// Writes are applied one at a time, each only after the previous one has
// finished (configBusy clearing): fixed spacing raced the app's own busy gate
// and silently dropped a request during the XCLK sweep, which left the device
// on the previous clock while the run claimed a new one.
//
// The set of settings has to be turned into data before it is walked. The
// chain used to be indexed directly, so the first setting a run did *not* ask
// for returned "nothing to apply" and the caller read that as the end of the
// list: `--quality` with no `--framesize` applied no quality at all, and every
// later option on the command line was dropped, while the run's result file
// still recorded the settings that were requested (CP-12). Asking for the
// slots up front means a gap cannot be mistaken for the end of the list, and
// "how many are left" is no longer derived from how many index steps happened
// to be taken.
class ConfigWriteSequence
{
public:
    ConfigWriteSequence() = default;

    // requested: index of each setting this run asked for, in the order the
    // settings are written - 0 framesize, 1 quality, 2 xclk, 3 frame-buffer
    // count, 4 grab mode.
    explicit ConfigWriteSequence(const QVector<int> &requested) : m_requested(requested)
    {
    }

    int total() const { return m_requested.size(); }
    // Issued so far, counted when the write is issued rather than when it
    // finishes - which is exactly when the busy handler asks "is there more".
    int applied() const { return m_applied; }
    bool hasNext() const { return m_applied < m_requested.size(); }

    // Index of the next setting to write, or -1 once every requested setting
    // has been issued. Calling it again past the end keeps returning -1.
    int takeNext()
    {
        if (!hasNext()) {
            return -1;
        }
        return m_requested.at(m_applied++);
    }

private:
    QVector<int> m_requested;
    int m_applied = 0;
};
