#ifndef AUDIOOUTPUT_HPP_
#define AUDIOOUTPUT_HPP_

#include <QAtomicInt>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include "audioring.hpp"

// Plays the decoded stream (S16LE, 48 kHz stereo) from an AudioRing through
// libasound, as media (the audio manager's "multimedia" type: headphones or
// speaker, the media volume keys, paused by calls).
//
// The ring is the jitter buffer: playback starts once kStartMs are queued and
// keeps the level under kMaxMs. A gateway that gets our acks sends up to 30 s
// ahead, so a weak signal does not interrupt the music. When the stream stops
// (end of the queue) the device runs dry and, after kReleaseMs, is closed so
// other apps get the audio back; new data reopens it. On pause (setHold) it
// stops at once and keeps what is queued for the resume.
class AudioOutput : public QThread
{
    Q_OBJECT
public:
    // (an enum, not static const ints: qMin/qMax take references, which
    // would need out-of-class definitions)
    enum {
        kRate = 48000,
        kBytesPerMs = kRate * 4 / 1000,
        kStartMs = 250,
        kMaxMs = 33000,  // the gateway's margin (30 s) and some slack
        kReleaseMs = 10000
    };

    explicit AudioOutput(AudioRing *source, QObject *parent = 0);
    void requestStop();
    // Paused: stop playing now (the device is closed) and keep the queue.
    void setHold(bool hold);
    QString error() const { return m_error; }

    // Stats, for the log.
    QAtomicInt underruns;
    QAtomicInt dropped;
    QAtomicInt deviceOpen;

signals:
    // The audio manager handle of the stream just opened (0: closed), for
    // NowPlayingConnection::setAudioManagerHandle. Emitted from this thread.
    void audioHandleChanged(unsigned int handle);
    // The output was reopened because the route changed (Bluetooth, headset).
    void rerouted();

protected:
    void run();

private:
    void waitWhileHeld();

    AudioRing *m_source;
    QAtomicInt m_stop;
    QAtomicInt m_reroute; // set by the route watcher
    QAtomicInt m_hold;
    QMutex m_holdMutex;
    QWaitCondition m_holdCond;
    QString m_error;
};

#endif /* AUDIOOUTPUT_HPP_ */
