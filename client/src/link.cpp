#include "link.hpp"

#include <QDebug>
#include <QTimer>
#include <QtEndian>

#include <opus.h>

#include "websocketclient.hpp"

namespace {
const int kMaxFrame = 5760;          // 120 ms at 48 kHz: Opus' longest frame
const int kRetryMs = 3000;
const int kLockedOutRetryMs = 5 * 60 * 1000;
const int kMaxConcealFrames = 5;     // fill at most 100 ms of a gap with Opus' concealment
const int kAckMs = 1000;
const int kFrameBytes = 960 * 4;     // one 20 ms frame, decoded
const int kDeviceFrames = 3;         // still in the device's own queue
}

Link::Link(AudioRing *ring, QObject *parent) :
        QObject(parent), m_ring(ring), m_ws(0), m_retry(0), m_ack(0), m_playing(false), m_decoder(0),
        m_pcm(kMaxFrame * 4, 0), m_lastSeq(-1), m_wanted(false)
{
}

Link::~Link()
{
    if (m_decoder) opus_decoder_destroy(m_decoder);
}

void Link::connectTo(const QUrl &url, const QByteArray &token)
{
    if (!m_ws) {
        // Created here, in the link's own thread.
        m_ws = new WebSocketClient(this);
        connect(m_ws, SIGNAL(opened()), this, SLOT(onOpened()));
        connect(m_ws, SIGNAL(closed(QString)), this, SLOT(onClosed(QString)));
        connect(m_ws, SIGNAL(textMessage(QByteArray)), this, SIGNAL(textReceived(QByteArray)));
        connect(m_ws, SIGNAL(binaryMessage(QByteArray)), this, SLOT(onBinary(QByteArray)));
        m_retry = new QTimer(this);
        m_retry->setSingleShot(true);
        connect(m_retry, SIGNAL(timeout()), this, SLOT(reconnect()));
        m_ack = new QTimer(this);
        m_ack->setInterval(kAckMs);
        connect(m_ack, SIGNAL(timeout()), this, SLOT(sendAck()));
        int err = 0;
        m_decoder = opus_decoder_create(48000, 2, &err);
        if (!m_decoder) qWarning() << "[Jukeberry:link] opus_decoder_create failed:" << err;
    }
    m_url = url;
    m_token = token;
    m_wanted = true;
    m_retry->stop();
    if (m_ws->isOpen()) m_ws->close(); // onClosed reconnects with the new settings
    else reconnect();
}

void Link::disconnectNow()
{
    m_wanted = false;
    if (m_retry) m_retry->stop();
    if (m_ws) m_ws->close();
}

void Link::reconnect()
{
    if (!m_wanted || !m_url.isValid() || m_token.isEmpty()) return;
    WebSocketClient::Headers headers;
    headers << qMakePair(QByteArray("Authorization"), "Bearer " + m_token);
    m_ws->open(m_url, headers);
}

void Link::sendText(const QByteArray &utf8)
{
    if (m_ws && m_ws->isOpen()) m_ws->sendText(utf8);
}

void Link::setPlaying(bool playing)
{
    m_playing = playing;
    sendAck(); // a resume: before the gateway reads on, so it keeps our margin
    if (!m_ack) return;
    if (playing && m_ws && m_ws->isOpen()) m_ack->start();
    else m_ack->stop();
}

// audio_ack {seq}: the last frame played = the newest one received minus
// what still waits in the ring and the device.
void Link::sendAck()
{
    if (m_lastSeq < 0 || !m_ws || !m_ws->isOpen()) return;
    const qint64 played = m_lastSeq - m_ring->level() / kFrameBytes - kDeviceFrames;
    sendText(QString("{\"cmd\":\"audio_ack\",\"seq\":%1}").arg(qMax(played, qint64(0))).toUtf8());
}

void Link::onOpened()
{
    // m_lastSeq stays: the gateway numbers its frames across connections, so
    // the first ack (sent with "audio on") lets it keep our margin instead of
    // starting over (a resume after the connection was closed in the
    // background would otherwise drop up to 30 s of queued music).
    if (m_playing) m_ack->start();
    emit opened();
}

void Link::onClosed(const QString &reason)
{
    if (m_ack) m_ack->stop();
    // WebSocketClient reports a refused upgrade as "upgrade refused: HTTP/1.1 401 ...".
    const bool auth = reason.contains(" 401 ");
    const bool locked = reason.contains(" 429 ");
    emit closed(reason, auth, locked);
    if (!m_wanted || auth) return;
    m_retry->start(locked ? kLockedOutRetryMs : kRetryMs);
}

// [0x01][flags][0][0][seq u32 LE][Opus packet]: 20 ms of 48 kHz stereo.
// [0x02][0][0][0][request id u32 LE][image]
void Link::onBinary(const QByteArray &data)
{
    if (data.size() < 8) return;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const quint32 word = qFromLittleEndian<quint32>(p + 4);
    if (p[0] == 0x02) {
        emit imageReceived(word, data.mid(8));
        return;
    }
    if (p[0] != 0x01 || !m_decoder) return;

    if (p[1] & 0x01) {
        // A seek, a new track or a restart: what is still queued is stale.
        m_ring->clear();
        discontinuities.ref();
        emit discontinuity();
    } else if (m_lastSeq >= 0 && qint64(word) > m_lastSeq + 1) {
        // Frames the gateway dropped for a slow connection.
        const int lost = int(qint64(word) - m_lastSeq - 1);
        framesLost.fetchAndAddRelaxed(lost);
        decodeLoss(qMin(lost, kMaxConcealFrames));
    }
    m_lastSeq = word;
    framesReceived.ref();

    const int n = opus_decode(m_decoder, p + 8, data.size() - 8,
                              reinterpret_cast<opus_int16 *>(m_pcm.data()), kMaxFrame, 0);
    if (n > 0) m_ring->write(m_pcm.constData(), n * 4);
}

void Link::decodeLoss(int frames)
{
    for (int i = 0; i < frames; ++i) {
        const int n = opus_decode(m_decoder, 0, 0, reinterpret_cast<opus_int16 *>(m_pcm.data()), 960, 0);
        if (n > 0) m_ring->write(m_pcm.constData(), n * 4);
    }
}
