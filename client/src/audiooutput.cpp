#include "audiooutput.hpp"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>

#include <string.h>
#include <sys/asoundlib.h>
#include <audio/audio_manager_routing.h>
#include <audio/audio_manager_event.h>
#include <errno.h>

namespace {

const int kFragMs = 20;
const int kFragBytes = AudioOutput::kBytesPerMs * kFragMs; // 3840

// The system's current output, from the audio service's PPS object
// ("path::/dev/snd/a2dpp" with a Bluetooth device).
QByteArray defaultOutputPath()
{
    QFile f("/pps/services/audio/devices/default");
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    foreach (const QByteArray &line, f.readAll().split('\n')) {
        if (line.startsWith("path::")) return line.mid(6).trimmed();
    }
    return QByteArray();
}

snd_pcm_t *openDevice(unsigned int *audioman, int *fragBytes, QString *error)
{
    snd_pcm_t *pcm = 0;
    int card = -1, device = 0;
    *audioman = 0;
    int rc;
    // "Preferred" is the phone's own audio chip even with Bluetooth on: the
    // DSP routes it to the A2DP device, which then never starts streaming.
    // The system's player writes to the A2DP device itself, and so do we.
    QByteArray path = defaultOutputPath();
    if (path.endsWith("a2dpp")) {
        rc = audio_manager_snd_pcm_open_name(AUDIO_TYPE_MULTIMEDIA, &pcm, audioman, path.data(),
                                             SND_PCM_OPEN_PLAYBACK);
    } else {
        rc = audio_manager_snd_pcm_open_preferred(AUDIO_TYPE_MULTIMEDIA, &pcm, audioman, &card, &device,
                                                  SND_PCM_OPEN_PLAYBACK);
    }
    if (rc < 0) {
        *error = QString("open: %1").arg(snd_strerror(rc));
        return 0;
    }
    // write() transfers, not mmap (mmap had 32-sample glitches on this phone,
    // see Berry Bridge's voice recorder).
    snd_pcm_plugin_set_disable(pcm, PLUGIN_MMAP);

    snd_pcm_channel_params_t params;
    memset(&params, 0, sizeof params);
    params.mode = SND_PCM_MODE_BLOCK;
    params.channel = SND_PCM_CHANNEL_PLAYBACK;
    params.start_mode = SND_PCM_START_FULL;
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = kFragBytes;
    params.buf.block.frags_min = 1;
    params.buf.block.frags_max = 4; // the latency stays in the ring, where it is measured
    params.format.interleave = 1;
    params.format.rate = AudioOutput::kRate;
    params.format.voices = 2;
    params.format.format = SND_PCM_SFMT_S16_LE;
    strncpy(params.sw_mixer_subchn_name, "Jukeberry", sizeof params.sw_mixer_subchn_name - 1);
    rc = snd_pcm_plugin_params(pcm, &params);
    if (rc < 0) {
        *error = QString("params: %1 (%2)").arg(snd_strerror(rc)).arg(params.why_failed);
    } else if ((rc = snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK)) < 0) {
        *error = QString("prepare: %1").arg(snd_strerror(rc));
    }
    if (!error->isEmpty()) {
        snd_pcm_close(pcm);
        if (*audioman) audio_manager_free_handle(*audioman);
        *audioman = 0;
        return 0;
    }
    snd_pcm_channel_setup_t setup;
    memset(&setup, 0, sizeof setup);
    setup.channel = SND_PCM_CHANNEL_PLAYBACK;
    snd_pcm_plugin_setup(pcm, &setup);
    *fragBytes = setup.buf.block.frag_size > 0 ? setup.buf.block.frag_size : kFragBytes;
    return pcm;
}

void closeDevice(snd_pcm_t *pcm, unsigned int audioman)
{
    snd_pcm_plugin_flush(pcm, SND_PCM_CHANNEL_PLAYBACK);
    snd_pcm_close(pcm);
    if (audioman) audio_manager_free_handle(audioman);
}

void recover(snd_pcm_t *pcm, QAtomicInt *underruns)
{
    snd_pcm_channel_status_t status;
    memset(&status, 0, sizeof status);
    status.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (snd_pcm_plugin_status(pcm, &status) == 0
            && (status.status == SND_PCM_STATUS_READY || status.status == SND_PCM_STATUS_UNDERRUN)) {
        if (status.status == SND_PCM_STATUS_UNDERRUN) underruns->ref();
        snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK);
    }
}

} // namespace

AudioOutput::AudioOutput(AudioRing *source, QObject *parent) :
        QThread(parent), m_source(source), m_stop(0)
{
}

// Waits for routing changes (a Bluetooth device or a headset connected or
// removed) on its own thread: audio_manager_get_event() blocks, and the event
// fd reads as ready even with nothing queued, so it can't be polled from the
// playback loop. A stream opened on the "preferred" output stays where it
// was, so the output thread reopens the device on the new route.
class RouteWatcher : public QThread
{
public:
    explicit RouteWatcher(QAtomicInt *flag) : m_flag(flag) {}
protected:
    void run()
    {
        audio_manager_event_context_t *events = 0;
        if (audio_manager_get_event_context(&events) != EOK || !events) return;
        if (audio_manager_add_routing_change_event(events) != EOK) {
            audio_manager_release_event_context(&events);
            return;
        }
        int last = -1;
        for (;;) {
            audio_manager_event_type_t type;
            void *params = 0;
            if (audio_manager_get_event(events, &type, &params) != EOK) {
                msleep(500);
                continue;
            }
            int now = -1;
            if (type == AUDIO_ROUTING_CHANGE && params)
                now = static_cast<audio_manager_routing_change_t *>(params)->dev_now;
            // Until dispatched, get_event returns the same event again at
            // once: the next change never arrived and this thread spun.
            audio_manager_dispatch_event(events, type, params);
            if (now < 0 || now == last) {
                msleep(100); // a repeat: never spin
                continue;
            }
            last = now;
            qDebug() << "[Jukeberry:audio] output device now" << now;
            *m_flag = 1;
        }
    }
private:
    QAtomicInt *m_flag;
};

void AudioOutput::run()
{
    const int startBytes = kStartMs * kBytesPerMs;
    const int maxBytes = kMaxMs * kBytesPerMs;
    // Lives as long as the app; it never returns from its wait, so it isn't
    // stopped or deleted.
    RouteWatcher *watcher = new RouteWatcher(&m_reroute);
    watcher->start();
    while (!m_stop) {
        // Idle: the device stays closed until a start buffer has arrived.
        if (!m_source->waitFor(startBytes, 200)) continue;

        unsigned int audioman = 0;
        int frag = 0;
        m_error.clear();
        snd_pcm_t *pcm = openDevice(&audioman, &frag, &m_error);
        if (!pcm) {
            qWarning() << "[Jukeberry:audio]" << m_error;
            msleep(1000);
            continue;
        }
        deviceOpen = 1;
        m_reroute = 0; // this open already uses the current route
        emit audioHandleChanged(audioman);
        QByteArray buffer(frag, 0);
        bool prebuffering = false;
        QElapsedTimer starved; // since the ring ran dry
        starved.invalidate();

        bool rerouted = false;
        while (!m_stop) {
            if (m_reroute) {
                rerouted = true;
                break; // reopen on the new route, the ring keeps its audio
            }
            if (prebuffering) {
                // Ran dry (pause, end of the queue, a slow network): wait for
                // a start buffer again, without writing; the device stops by
                // itself. Long enough idle: give the audio back.
                if (!m_source->waitFor(startBytes, 50)) {
                    if (starved.isValid() && starved.elapsed() > kReleaseMs) break;
                    continue;
                }
                prebuffering = false;
                starved.invalidate();
            }
            // Bounded latency: a burst, or the sender's clock running fast.
            while (m_source->level() > maxBytes) {
                m_source->skip(frag);
                dropped.ref();
            }
            if (!m_source->waitFor(frag, 2 * kFragMs)) {
                if (m_source->level() == 0) {
                    prebuffering = true;
                    starved.start();
                    continue;
                }
            }
            const int got = m_source->read(buffer.data(), frag);
            if (got < frag) memset(buffer.data() + got, 0, frag - got);
            const ssize_t n = snd_pcm_plugin_write(pcm, buffer.constData(), frag);
            if (n < frag) recover(pcm, &underruns);
        }
        closeDevice(pcm, audioman);
        deviceOpen = 0;
        emit audioHandleChanged(0);
        if (rerouted) {
            qDebug() << "[Jukeberry:audio] route changed, reopening";
            emit this->rerouted();
        }
    }
}
