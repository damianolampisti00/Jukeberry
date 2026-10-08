#ifndef LINK_HPP_
#define LINK_HPP_

#include <QAtomicInt>
#include <QByteArray>
#include <QObject>
#include <QUrl>

#include "audioring.hpp"

class QTimer;
class WebSocketClient;
struct OpusDecoder;

// The connection to the Jukeberry gateway (server/gateway.go), on its own
// thread so the UI can never hold up the audio: binary Opus frames are
// decoded straight into the playback ring here; text (replies, events) and
// images go to the UI thread as signals.
//
// It reconnects by itself, except after a 401: retrying a wrong token would
// only lock this network out of the gateway (10 failures), so it waits for
// new settings instead.
class Link : public QObject
{
    Q_OBJECT
public:
    explicit Link(AudioRing *ring, QObject *parent = 0);
    ~Link();

    // Stats (any thread).
    QAtomicInt framesReceived, framesLost, discontinuities;

public slots:
    void connectTo(const QUrl &url, const QByteArray &token);
    void disconnectNow();
    void sendText(const QByteArray &utf8);

signals:
    void opened();
    // authFailed: the token was refused; lockedOut: too many wrong tokens.
    void closed(const QString &reason, bool authFailed, bool lockedOut);
    void textReceived(const QByteArray &utf8);
    void imageReceived(quint32 requestId, const QByteArray &image);

private slots:
    void onOpened();
    void onClosed(const QString &reason);
    void onBinary(const QByteArray &data);
    void reconnect();

private:
    void decodeLoss(int frames);

    AudioRing *m_ring;
    WebSocketClient *m_ws;
    QTimer *m_retry;
    QUrl m_url;
    QByteArray m_token;
    OpusDecoder *m_decoder;
    QByteArray m_pcm;
    qint64 m_lastSeq;
    bool m_wanted;
};

#endif /* LINK_HPP_ */
