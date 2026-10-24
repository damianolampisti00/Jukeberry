#include "player.hpp"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>

#include <bb/cascades/Application>
#include <bb/data/JsonDataAccess>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/MetaData>
#include <bb/multimedia/NowPlayingConnection>
#include <bb/multimedia/OverlayStyle>

#include "audiooutput.hpp"
#include "link.hpp"

namespace {
// Paused in the background this long: the connection is closed.
const int kBackgroundCloseMs = 30000;
}

using namespace bb::cascades;
using bb::multimedia::NowPlayingConnection;

namespace {

// The gateway sends up to 30 s ahead of what we have played (our acks), so
// a weak signal does not interrupt the music: ~6.7 MB of decoded audio.
const int kRingBytes = AudioOutput::kBytesPerMs * 35000;
// A track change further ahead than this is shown when the audio gets there.
const int kShowTrackLaterMs = 1500;
const int kDeviceLatencyMs = 60;                        // the device's own queue (frags_max 4 x 20 ms, part full)
const int kContextPollMs = 1000;
const int kMaxContextPolls = 30;
const int kMaxThumbs = 200;

QString settingsOrg() { return QString("Jukeberry"); }

// Spotify's cover URLs name their size: 1e02 = 300 px, b273 = 640 px,
// 4851 = 64 px. Lists only need the small one (a few KB instead of ~30).
QString thumbUrl(const QString &url)
{
    QString u = url;
    u.replace("ab67616d00001e02", "ab67616d00004851");
    u.replace("ab67616d0000b273", "ab67616d00004851");
    return u;
}

QString joinStrings(const QVariant &list)
{
    QStringList out;
    foreach (const QVariant &v, list.toList()) out << v.toString();
    return out.join(", ");
}

} // namespace

Player::Player(QObject *parent) :
        QObject(parent),
        m_connected(false),
        m_ring(kRingBytes),
        m_link(0), m_audio(0),
        m_nextId(0),
        m_duration(0), m_posBase(0),
        m_playing(false), m_liked(false), m_shuffle(false), m_repeat(0),
        m_tracksLoading(false), m_playlistsLoading(false), m_searching(false),
        m_queueLoading(false), m_queueVisible(false),
        m_contextPolls(0),
        m_np(0),
        m_background(false),
        m_bgClosed(false),
        m_resumeOnOpen(false),
        m_hasPending(false)
{
    qRegisterMetaType<QUrl>("QUrl");

    QSettings settings(settingsOrg(), settingsOrg());
    m_serverUrl = settings.value("serverUrl").toString();
    m_token = settings.value("token").toString();

    m_artDir = QDir::homePath() + "/art";
    QDir().mkpath(m_artDir);

    m_playlists = new ArrayDataModel(this);
    m_tracks = new ArrayDataModel(this);
    m_results = new ArrayDataModel(this);
    m_queue = new ArrayDataModel(this);

    // The position display: only while playing (it woke the phone twice a
    // second all the time the app was open).
    m_tick.setInterval(500);
    connect(&m_tick, SIGNAL(timeout()), this, SLOT(onTick()));
    m_bgTimer.setSingleShot(true);
    connect(&m_bgTimer, SIGNAL(timeout()), this, SLOT(onBackgroundIdle()));
    connect(bb::cascades::Application::instance(), SIGNAL(thumbnail()), this, SLOT(onAppBackground()));
    connect(bb::cascades::Application::instance(), SIGNAL(invisible()), this, SLOT(onAppBackground()));
    connect(bb::cascades::Application::instance(), SIGNAL(fullscreen()), this, SLOT(onAppForeground()));
    m_contextTimer.setSingleShot(true);
    connect(&m_contextTimer, SIGNAL(timeout()), this, SLOT(pollContext()));

    // The network on its own thread, the audio on another.
    m_link = new Link(&m_ring);
    m_link->moveToThread(&m_linkThread);
    connect(this, SIGNAL(linkConnect(QUrl, QByteArray)), m_link, SLOT(connectTo(QUrl, QByteArray)));
    connect(this, SIGNAL(linkSend(QByteArray)), m_link, SLOT(sendText(QByteArray)));
    connect(this, SIGNAL(linkDisconnect()), m_link, SLOT(disconnectNow()));
    connect(m_link, SIGNAL(opened()), this, SLOT(onOpened()));
    connect(m_link, SIGNAL(closed(QString, bool, bool)), this, SLOT(onClosed(QString, bool, bool)));
    connect(m_link, SIGNAL(textReceived(QByteArray)), this, SLOT(onText(QByteArray)));
    connect(m_link, SIGNAL(imageReceived(quint32, QByteArray)), this, SLOT(onImage(quint32, QByteArray)));
    connect(this, SIGNAL(linkPlaying(bool)), m_link, SLOT(setPlaying(bool)));
    connect(this, SIGNAL(linkSkipTo(quint32)), m_link, SLOT(skipTo(quint32)));
    connect(m_link, SIGNAL(discontinuity()), this, SLOT(onDiscontinuity()));
    m_pendingTimer.setSingleShot(true);
    connect(&m_pendingTimer, SIGNAL(timeout()), this, SLOT(applyPendingTrack()));
    m_linkThread.start();
    m_audio = new AudioOutput(&m_ring);
    m_audio->start(QThread::TimeCriticalPriority);

    // Lock screen, headset buttons and the system's media overlay.
    m_np = new NowPlayingConnection("Jukeberry", this);
    m_np->setOverlayStyle(bb::multimedia::OverlayStyle::Fancy);
    m_np->setNextEnabled(true);
    m_np->setPreviousEnabled(true);
    connect(m_np, SIGNAL(play()), this, SLOT(onNpPlay()));
    connect(m_np, SIGNAL(pause()), this, SLOT(onNpPause()));
    connect(m_np, SIGNAL(next()), this, SLOT(next()));
    connect(m_np, SIGNAL(previous()), this, SLOT(previous()));
    connect(m_np, SIGNAL(revoked()), this, SLOT(onNpRevoked()));
    // Tie the controls to our audio stream, so the system knows which output
    // is ours (calls, Bluetooth, other players) instead of guessing.
    connect(m_audio, SIGNAL(audioHandleChanged(unsigned int)), this, SLOT(onAudioHandle(unsigned int)));

    if (configured()) connectNow();
    else setMessage(tr("Enter your Jukeberry server in Settings"));
}

Player::~Player()
{
    m_audio->requestStop();
    m_audio->wait(2000);
    delete m_audio;
    QMetaObject::invokeMethod(m_link, "deleteLater");
    m_linkThread.quit();
    m_linkThread.wait(2000);
}

// --- connection ------------------------------------------------------------

void Player::saveSettings(const QString &serverUrl, const QString &token)
{
    m_serverUrl = serverUrl.trimmed();
    m_token = token.trimmed();
    QSettings settings(settingsOrg(), settingsOrg());
    settings.setValue("serverUrl", m_serverUrl);
    settings.setValue("token", m_token);
    emit settingsChanged();
    if (configured()) connectNow();
}

void Player::connectNow()
{
    QString u = m_serverUrl;
    if (!u.contains("://")) u = "https://" + u;
    QUrl url(u);
    if (url.scheme() == "https") url.setScheme("wss");
    else if (url.scheme() == "http") url.setScheme("ws");
    if (url.path().isEmpty() || url.path() == "/") url.setPath("/ws");
    setMessage(tr("Connecting..."));
    emit linkConnect(url, m_token.toUtf8());
}

void Player::onOpened()
{
    m_connected = true;
    emit connectedChanged();
    setMessage(QString());
    send("status", QVariantMap(), Status);
    QVariantMap audio;
    audio["on"] = true;
    send("audio", audio);
    emit linkPlaying(m_playing); // an ack at once: the gateway keeps our queued audio
    if (m_resumeOnOpen) { // play was pressed while the connection was closed
        m_resumeOnOpen = false;
        setPlaying(true); // (its ack goes before the resume)
        send("resume");
    }
    if (m_playlists->isEmpty()) loadPlaylists();
}

void Player::onClosed(const QString &reason, bool authFailed, bool lockedOut)
{
    qDebug() << "[Jukeberry] connection closed:" << reason;
    const bool was = m_connected;
    m_connected = false;
    if (was) emit connectedChanged();
    m_pending.clear();
    m_artInFlight.clear();
    if (m_bgClosed) setMessage(QString()); // closed on purpose (onBackgroundIdle)
    else if (authFailed) setMessage(tr("Invalid token: check Settings"));
    else if (lockedOut) setMessage(tr("Too many wrong tokens from this network: the server blocks it for 15 minutes"));
    else setMessage(tr("Connection lost, retrying..."));
}

void Player::setMessage(const QString &m)
{
    if (m == m_message) return;
    m_message = m;
    emit messageChanged();
}

qint64 Player::send(const QString &cmd, const QVariantMap &args, Purpose purpose, const QString &key)
{
    if (!m_connected) {
        if (purpose == None) setMessage(tr("Not connected to the server"));
        return 0;
    }
    const qint64 id = ++m_nextId;
    QVariantMap m = args;
    m["id"] = id;
    m["cmd"] = cmd;
    QByteArray json;
    bb::data::JsonDataAccess().saveToBuffer(QVariant(m), &json);
    if (purpose != None) {
        Pending p;
        p.purpose = purpose;
        p.key = key;
        m_pending.insert(id, p);
    }
    emit linkSend(json);
    return id;
}

// --- messages from the gateway ---------------------------------------------

void Player::onText(const QByteArray &utf8)
{
    const QVariantMap m = bb::data::JsonDataAccess().loadFromBuffer(utf8).toMap();
    if (m.contains("event")) {
        onEvent(m.value("event").toString(), m.value("data").toMap());
        return;
    }
    const qint64 id = m.value("id").toLongLong();
    const Pending p = m_pending.take(id);
    const bool ok = m.value("ok").toBool();
    const QVariant data = m.value("data");
    if (!ok) {
        qDebug() << "[Jukeberry] command" << id << "failed:" << m.value("error").toString();
    }
    switch (p.purpose) {
    case Status:
        if (ok) applyStatus(data.toMap());
        break;
    case Playlists: {
        m_playlistsLoading = false;
        m_playlists->clear();
        QVariantMap liked;
        liked["kind"] = "playlist";
        liked["uri"] = "spotify:collection:tracks";
        liked["name"] = tr("Liked Songs");
        liked["thumb"] = "asset:///images/ic_liked.png";
        m_playlists->append(liked);
        int thumbs = 0;
        foreach (const QVariant &v, data.toMap().value("items").toList()) {
            const QVariantMap pl = v.toMap();
            QVariantMap item;
            item["kind"] = "playlist";
            item["uri"] = pl.value("uri");
            const QString name = pl.value("name").toString();
            item["name"] = name.isEmpty() ? tr("Untitled playlist") : name;
            item["subtitle"] = tr("%1 tracks").arg(pl.value("length").toInt());
            item["canEdit"] = pl.value("can_edit").toBool();
            const QString image = thumbUrl(pl.value("image_url").toString());
            item["image"] = image;
            item["thumb"] = artFile(image);
            if (!image.isEmpty() && thumbs++ < kMaxThumbs) requestArt(image);
            m_playlists->append(item);
        }
        emit playlistsChanged();
        break;
    }
    case Context:
        if (p.key == m_tracksUri) {
            if (ok) fillContext(data.toMap());
            else {
                m_tracksLoading = false;
                emit tracksChanged();
                setMessage(m.value("error").toString());
            }
        }
        break;
    case Search:
        m_searching = false;
        if (ok) fillResults(data.toMap());
        else setMessage(m.value("error").toString());
        emit resultsChanged();
        break;
    case Queue:
        m_queueLoading = false;
        if (ok) fillQueue(data.toMap());
        emit queueChanged();
        break;
    case LikedList:
        if (ok) {
            foreach (const QVariant &v, data.toMap().value("items").toList()) {
                const QVariantMap s = v.toMap();
                m_likedCache.insert(s.value("uri").toString(), s.value("liked").toBool());
            }
            applyLikedCache();
        }
        break;
    case PlaylistAdd:
        if (ok) emit notice(tr("Added to %1").arg(p.key));
        else setMessage(tr("Couldn't add to %1").arg(p.key));
        break;
    case Liked:
        foreach (const QVariant &v, data.toMap().value("items").toList()) {
            const QVariantMap s = v.toMap();
            if (s.value("uri").toString() == m_trackUri) {
                m_liked = s.value("liked").toBool();
                emit likedChanged();
            }
        }
        break;
    case Art:
        m_artInFlight.remove(p.key); // only errors arrive here: images come as binary
        break;
    case None:
        if (!ok) setMessage(m.value("error").toString());
        break;
    }
}

void Player::onEvent(const QString &type, const QVariantMap &data)
{
    if (type == "metadata") {
        // The gateway reads a natural track change as soon as its margin
        // reaches it: show it when our audio gets there. (A jump empties
        // our queue first or soon after: onDiscontinuity shows it at once.)
        const qint64 lag = lagMs();
        if (lag > kShowTrackLaterMs && !m_trackUri.isEmpty()
                && data.value("uri").toString() != m_trackUri) {
            m_pendingTrack = data;
            m_hasPending = true;
            m_pendingTimer.start(int(lag));
        } else {
            applyMetadata(data, lag);
        }
    } else if (type == "playing") {
        setPlaying(true);
    } else if (type == "paused") {
        setPlaying(false);
    } else if (type == "stopped") {
        // The end of the queue, which the gateway reaches up to 30 s before
        // we do: let the queued end of the last track play out.
        setPlaying(false);
        if (m_audio) m_audio->setHold(false);
    } else if (type == "skip_to") {
        // "next" while the gateway already had the next track's start: jump
        // to it in our queue (no new track from Spotify)
        emit linkSkipTo(data.value("seq").toUInt());
        applyPendingTrack();
    } else if (type == "seek") {
        // a seek empties our queue: what plays next is this position
        setPositionBase(data.value("position").toLongLong());
    } else if (type == "shuffle_context") {
        m_shuffle = data.value("value").toBool();
        emit modesChanged();
    } else if (type == "repeat_context") {
        if (data.value("value").toBool()) m_repeat = qMax(m_repeat, 1);
        else if (m_repeat == 1) m_repeat = 0;
        emit modesChanged();
    } else if (type == "repeat_track") {
        if (data.value("value").toBool()) m_repeat = 2;
        else if (m_repeat == 2) m_repeat = 0;
        emit modesChanged();
    } else if (type == "playback_error") {
        setMessage(tr("This track can't be played"));
    }
}

void Player::applyStatus(const QVariantMap &s)
{
    m_shuffle = s.value("shuffle_context").toBool();
    m_repeat = s.value("repeat_track").toBool() ? 2 : (s.value("repeat_context").toBool() ? 1 : 0);
    emit modesChanged();
    m_contextName = s.value("context_name").toString();
    const QVariantMap track = s.value("track").toMap();
    if (!track.isEmpty()) {
        m_hasPending = false;
        m_pendingTimer.stop();
        applyTrack(track);
        setPositionBase(track.value("position").toLongLong() - lagMs());
    }
    setPlaying(!s.value("paused").toBool() && !s.value("stopped").toBool() && !track.isEmpty());
}

void Player::applyTrack(const QVariantMap &t)
{
    m_trackUri = t.value("uri").toString();
    m_title = t.value("name").toString();
    m_artists = joinStrings(t.value("artist_names"));
    m_album = t.value("album_name").toString();
    m_duration = t.value("duration").toInt();
    m_coverUrl = t.value("album_cover_url").toString();
    m_cover = artFile(m_coverUrl);
    if (m_cover.isEmpty()) requestArt(m_coverUrl);
    emit trackChanged();
    emit coverChanged();

    m_liked = false;
    emit likedChanged();
    if (!m_trackUri.isEmpty()) {
        QVariantMap args;
        args["uris"] = QVariantList() << m_trackUri;
        send("liked", args, Liked);
    }
    updateNowPlaying();
}

// --- playback position -----------------------------------------------------

// m_posBase is what was audible when m_posClock restarted: go-librespot
// reports what it has handed out, so the bases are set minus our queue
// (lagMs) at that moment. (Subtracting the queue at every display, as before,
// went wrong once the gateway sends in bursts to keep a 30 s margin.)
int Player::position() const
{
    qint64 ms = m_posBase;
    if (m_playing && m_posClock.isValid()) ms += m_posClock.elapsed();
    if (ms < 0) ms = 0;
    if (m_duration > 0 && ms > m_duration) ms = m_duration;
    return int(ms);
}

qint64 Player::lagMs() const
{
    return m_ring.level() / AudioOutput::kBytesPerMs + kDeviceLatencyMs;
}

void Player::applyMetadata(const QVariantMap &track, qint64 lag)
{
    m_hasPending = false;
    m_pendingTimer.stop();
    applyTrack(track);
    setPositionBase(track.value("position").toLongLong() - lag);
    if (m_queueVisible) loadQueue(); // the queue moved on
}

// the audio reached the track change the gateway announced earlier
void Player::applyPendingTrack()
{
    if (m_hasPending) applyMetadata(m_pendingTrack, 0);
}

// a seek or a jump emptied our queue: an announced track plays now
void Player::onDiscontinuity()
{
    if (m_hasPending) applyMetadata(m_pendingTrack, lagMs());
}

void Player::setPositionBase(qint64 ms)
{
    m_posBase = ms;
    m_posClock.restart();
    emit positionChanged();
}

void Player::setPlaying(bool playing)
{
    // Paused: our audio stops at once and keeps its queue (up to 30 s) for
    // the resume; the link acks only while playing.
    if (m_audio) m_audio->setHold(!playing);
    if (playing == m_playing) return;
    emit linkPlaying(playing);
    if (playing) {
        m_posClock.restart();
    } else if (m_posClock.isValid()) {
        m_posBase += m_posClock.elapsed();
    }
    m_playing = playing;
    if (playing) m_tick.start();
    else m_tick.stop();
    // Paused in the background: the connection goes after a while too.
    if (playing) m_bgTimer.stop();
    else if (m_background) m_bgTimer.start(kBackgroundCloseMs);
    emit playStateChanged();
    emit positionChanged();
    updateNowPlaying();
}

// Minimized or covered while nothing plays: after kBackgroundCloseMs the
// connection to the server is closed (no keepalives, no events: the radio
// can sleep). Playing, it stays (the audio comes over it). Back in full
// screen, or play pressed on the lock screen or a headset, it reopens.
void Player::onAppBackground()
{
    if (m_background) return;
    m_background = true;
    if (!m_playing) m_bgTimer.start(kBackgroundCloseMs);
}

void Player::onBackgroundIdle()
{
    if (!m_background || m_playing || m_bgClosed || !m_connected) return;
    qDebug() << "[Jukeberry] in the background and paused: closing the connection";
    m_bgClosed = true;
    emit linkDisconnect();
}

void Player::onAppForeground()
{
    if (!m_background) return;
    m_background = false;
    m_bgTimer.stop();
    if (m_bgClosed) {
        m_bgClosed = false;
        if (configured()) connectNow();
    }
}

void Player::onTick()
{
    if (m_playing) emit positionChanged();
}

QString Player::formatTime(int ms) const
{
    const int s = qMax(0, ms / 1000);
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// --- actions ---------------------------------------------------------------

void Player::playPause()
{
    if (!hasTrack()) return;
    const bool resume = !m_playing;
    setPlaying(resume); // at once (and a resume's ack goes first); the event confirms it
    send(resume ? "resume" : "pause");
}

void Player::next() { send("next"); }
void Player::previous() { send("prev"); }

void Player::seekTo(int ms)
{
    QVariantMap args;
    args["position"] = ms;
    send("seek", args);
    setPositionBase(ms);
}

void Player::toggleShuffle()
{
    QVariantMap args;
    args["value"] = !m_shuffle;
    send("shuffle", args);
    m_shuffle = !m_shuffle;
    emit modesChanged();
}

void Player::cycleRepeat()
{
    QVariantMap on;
    on["value"] = true;
    QVariantMap off;
    off["value"] = false;
    if (m_repeat == 0) {
        send("repeat_context", on);
        m_repeat = 1;
    } else if (m_repeat == 1) {
        send("repeat_track", on);
        m_repeat = 2;
    } else {
        send("repeat_track", off);
        send("repeat_context", off);
        m_repeat = 0;
    }
    emit modesChanged();
}

void Player::toggleLike()
{
    if (!hasTrack()) return;
    QVariantMap args;
    args["uris"] = QVariantList() << m_trackUri;
    args["liked"] = !m_liked;
    send("like", args);
    m_liked = !m_liked;
    emit likedChanged();
}

void Player::play(const QString &uri, const QString &skipTo)
{
    QVariantMap args;
    args["uri"] = uri;
    if (!skipTo.isEmpty()) args["skip_to_uri"] = skipTo;
    send("play", args);
}

// --- library and search ----------------------------------------------------

void Player::loadPlaylists()
{
    QVariantMap args;
    args["limit"] = 200;
    if (send("playlists", args, Playlists)) {
        m_playlistsLoading = true;
        emit playlistsChanged();
    }
}

void Player::openContext(const QString &uri, const QString &title)
{
    m_tracksUri = uri;
    m_tracksTitle = title;
    m_tracks->clear();
    m_contextPolls = 0;
    m_contextTimer.stop();
    QVariantMap args;
    args["uri"] = uri;
    m_tracksLoading = send("context", args, Context, uri) != 0;
    emit tracksChanged();
}

void Player::pollContext()
{
    if (m_tracksUri.isEmpty()) return;
    QVariantMap args;
    args["uri"] = m_tracksUri;
    send("context", args, Context, m_tracksUri);
}

// go-librespot enumerates a context in the background: until `ready` the
// listing is empty, then entries fill in (track null) as metadata arrives.
void Player::fillContext(const QVariantMap &d)
{
    const bool ready = d.value("ready").toBool();
    const int length = d.value("length").toInt();
    const int cached = d.value("cached").toInt();
    const QVariantList entries = d.value("tracks").toList();
    const bool rebuild = m_tracks->size() != entries.size();
    if (rebuild) m_tracks->clear();
    int thumbs = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const QVariantMap e = entries.at(i).toMap();
        const QVariantMap t = e.value("track").toMap();
        QVariantMap item;
        item["kind"] = "track";
        item["uri"] = e.value("uri");
        item["context"] = m_tracksUri;
        item["liked"] = (m_tracksUri == "spotify:collection:tracks")
                        || m_likedCache.value(e.value("uri").toString(), false);
        item["name"] = t.isEmpty() ? tr("Loading...") : t.value("name").toString();
        item["subtitle"] = joinStrings(t.value("artist_names"));
        item["duration"] = formatTime(t.value("duration").toInt());
        const QString image = thumbUrl(t.value("album_cover_url").toString());
        item["image"] = image;
        item["thumb"] = artFile(image);
        if (!image.isEmpty() && item["thumb"].toString().isEmpty() && thumbs++ < kMaxThumbs) requestArt(image);
        if (rebuild) m_tracks->append(item);
        else if (m_tracks->value(i).toMap().value("name") != item["name"]) m_tracks->replace(i, item);
    }
    const bool more = (!ready || cached < length) && ++m_contextPolls < kMaxContextPolls;
    if (more) m_contextTimer.start(kContextPollMs);
    m_tracksLoading = !ready;
    if (ready && m_tracksUri != "spotify:collection:tracks") requestLiked(m_tracks);
    emit tracksChanged();
}

void Player::search(const QString &query)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) return;
    m_results->clear();
    QVariantMap args;
    args["q"] = q;
    args["limit"] = 10;
    m_searching = send("search", args, Search) != 0;
    emit resultsChanged();
}

void Player::fillResults(const QVariantMap &d)
{
    m_results->clear();
    const char *kinds[] = { "tracks", "albums", "artists", "playlists" };
    const QString titles[] = { tr("Songs"), tr("Albums"), tr("Artists"), tr("Playlists") };
    for (int k = 0; k < 4; ++k) {
        const QVariantList items = d.value(kinds[k]).toList();
        if (items.isEmpty()) continue;
        QVariantMap header;
        header["kind"] = "header";
        header["name"] = titles[k];
        m_results->append(header);
        foreach (const QVariant &v, items) {
            QVariantMap item = v.toMap(); // kind, uri, name, subtitle, image, duration
            const QString image = thumbUrl(item.value("image").toString());
            item["image"] = image;
            item["thumb"] = artFile(image);
            if (item.contains("duration")) item["duration"] = formatTime(item.value("duration").toInt());
            if (!image.isEmpty()) requestArt(image);
            if (item.value("kind") == "track") item["liked"] = m_likedCache.value(item.value("uri").toString(), false);
            m_results->append(item);
        }
    }
    requestLiked(m_results);
}

// Liked state of a list's tracks, for its "Add to / Remove from Liked Songs"
// action: the ones not known yet, asked 50 at a time.
void Player::requestLiked(bb::cascades::ArrayDataModel *model)
{
    QVariantList batch;
    for (int i = 0; i < model->size(); ++i) {
        const QVariantMap item = model->value(i).toMap();
        const QString uri = item.value("uri").toString();
        if (item.value("kind") != "track" || !uri.startsWith("spotify:track:") || m_likedCache.contains(uri)) continue;
        batch << uri;
        if (batch.size() == 50) {
            QVariantMap args;
            args["uris"] = batch;
            send("liked", args, LikedList);
            batch.clear();
        }
    }
    if (!batch.isEmpty()) {
        QVariantMap args;
        args["uris"] = batch;
        send("liked", args, LikedList);
    }
}

void Player::applyLikedCache()
{
    bb::cascades::ArrayDataModel *models[] = { m_tracks, m_results };
    for (int m = 0; m < 2; ++m) {
        for (int i = 0; i < models[m]->size(); ++i) {
            QVariantMap item = models[m]->value(i).toMap();
            const QString uri = item.value("uri").toString();
            if (item.value("kind") != "track" || !m_likedCache.contains(uri)) continue;
            const bool liked = m_likedCache.value(uri);
            if (item.value("liked").toBool() == liked) continue;
            item["liked"] = liked;
            models[m]->replace(i, item);
        }
    }
}

// --- play queue ------------------------------------------------------------

void Player::loadQueue()
{
    if (send("upcoming", QVariantMap(), Queue)) {
        m_queueLoading = true;
        emit queueChanged();
    }
}

void Player::setQueueVisible(bool visible)
{
    m_queueVisible = visible;
    if (visible) loadQueue();
}

void Player::skipTo(const QString &uri)
{
    QVariantMap args;
    args["uri"] = uri;
    send("next", args);
}

void Player::setLiked(const QString &uri, bool liked)
{
    QVariantMap args;
    args["uris"] = QVariantList() << uri;
    args["liked"] = liked;
    if (!send("like", args)) return;
    m_likedCache.insert(uri, liked);
    applyLikedCache();
    if (uri == m_trackUri && m_liked != liked) {
        m_liked = liked;
        emit likedChanged();
    }
    emit notice(liked ? tr("Added to Liked Songs") : tr("Removed from Liked Songs"));
}

QVariantList Player::editablePlaylists() const
{
    QVariantList list;
    for (int i = 0; i < m_playlists->size(); ++i) {
        const QVariantMap item = m_playlists->value(i).toMap();
        if (!item.value("canEdit").toBool()) continue;
        QVariantMap p;
        p["name"] = item.value("name");
        p["uri"] = item.value("uri");
        list << p;
    }
    return list;
}

void Player::addToPlaylist(const QString &playlistUri, const QString &playlistName, const QString &trackUri)
{
    QVariantMap args;
    args["uri"] = playlistUri;
    args["uris"] = QVariantList() << trackUri;
    send("playlistadd", args, PlaylistAdd, playlistName);
}

// The queue's queued entries (uids, in order), changed and sent back whole:
// the gateway's setqueue keeps those, in that order, and drops the others.
QStringList Player::queuedUids() const
{
    QStringList uids;
    for (int i = 0; i < m_queue->size(); ++i) {
        const QVariantMap item = m_queue->value(i).toMap();
        if (item.value("queued").toBool() && !item.value("uid").toString().isEmpty())
            uids << item.value("uid").toString();
    }
    return uids;
}

void Player::sendQueue(const QStringList &uids)
{
    QVariantMap args;
    args["uids"] = QVariant(uids);
    send("setqueue", args);
    QTimer::singleShot(700, this, SLOT(loadQueue())); // the player's own view of it
}

void Player::removeFromQueue(const QString &uid)
{
    QStringList uids = queuedUids();
    uids.removeAll(uid);
    for (int i = 0; i < m_queue->size(); ++i) {
        if (m_queue->value(i).toMap().value("uid").toString() == uid) {
            m_queue->removeAt(i);
            break;
        }
    }
    sendQueue(uids);
}

void Player::moveInQueue(const QString &uid, int delta)
{
    QStringList uids = queuedUids();
    const int from = uids.indexOf(uid);
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= uids.size()) return;
    uids.move(from, to);
    // Queued entries come first in the model, in the same order.
    m_queue->move(from, to);
    sendQueue(uids);
}

void Player::clearQueue()
{
    for (int i = m_queue->size() - 1; i >= 0; --i) {
        if (m_queue->value(i).toMap().value("queued").toBool()) m_queue->removeAt(i);
    }
    sendQueue(QStringList());
    emit queueChanged();
}

void Player::addToQueue(const QString &uri)
{
    QVariantMap args;
    args["uri"] = uri;
    if (send("queue", args)) {
        emit queued();
        if (m_queueVisible) QTimer::singleShot(800, this, SLOT(loadQueue()));
    }
}

// The gateway's "upcoming": {length, items: [{uri, provider, track|null}]};
// tracks added to the queue come first (provider "queue").
void Player::fillQueue(const QVariantMap &d)
{
    m_queue->clear();
    foreach (const QVariant &v, d.value("items").toList()) {
        const QVariantMap e = v.toMap();
        const QVariantMap t = e.value("track").toMap();
        QVariantMap item;
        item["kind"] = "track";
        item["uri"] = e.value("uri");
        item["queued"] = e.value("provider").toString() == "queue";
        item["uid"] = e.value("uid");
        item["name"] = t.isEmpty() ? tr("Loading...") : t.value("name").toString();
        item["subtitle"] = joinStrings(t.value("artist_names"));
        item["duration"] = formatTime(t.value("duration").toInt());
        const QString image = thumbUrl(t.value("album_cover_url").toString());
        item["image"] = image;
        item["thumb"] = artFile(image);
        if (!image.isEmpty() && item["thumb"].toString().isEmpty()) requestArt(image);
        m_queue->append(item);
    }
}

// --- cover art -------------------------------------------------------------

QString Player::artFile(const QString &url) const
{
    if (url.isEmpty()) return QString();
    const QString path = m_artDir + "/"
            + QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex() + ".jpg";
    return QFile::exists(path) ? "file://" + path : QString();
}

void Player::requestArt(const QString &url)
{
    if (url.isEmpty() || m_artInFlight.contains(url) || !artFile(url).isEmpty()) return;
    QVariantMap args;
    args["url"] = url;
    if (send("art", args, Art, url)) m_artInFlight.insert(url, true);
}

void Player::onImage(quint32 requestId, const QByteArray &image)
{
    const Pending p = m_pending.take(requestId);
    if (p.purpose != Art) return;
    m_artInFlight.remove(p.key);
    const QString path = m_artDir + "/"
            + QCryptographicHash::hash(p.key.toUtf8(), QCryptographicHash::Md5).toHex() + ".jpg";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(image) != image.size()) return;
    f.close();
    applyArt(p.key, "file://" + path);
}

void Player::applyArt(const QString &url, const QString &file)
{
    if (url == m_coverUrl) {
        m_cover = file;
        emit coverChanged();
        updateNowPlaying();
    }
    ArrayDataModel *models[] = { m_playlists, m_tracks, m_results, m_queue };
    for (int k = 0; k < 4; ++k) {
        ArrayDataModel *model = models[k];
        for (int i = 0; i < model->size(); ++i) {
            QVariantMap item = model->value(i).toMap();
            if (item.value("image").toString() == url && item.value("thumb").toString() != file) {
                item["thumb"] = file;
                model->replace(i, item);
            }
        }
    }
}

// --- lock screen / headset -------------------------------------------------

void Player::updateNowPlaying()
{
    if (!hasTrack()) return;
    if (m_playing && !m_np->isAcquired()) m_np->acquire();
    if (!m_np->isAcquired()) return;
    QVariantMap meta;
    meta[bb::multimedia::MetaData::Title] = m_title;
    meta[bb::multimedia::MetaData::Artist] = m_artists;
    meta[bb::multimedia::MetaData::Album] = m_album;
    m_np->setMetaData(meta);
    m_np->setDuration(m_duration);
    if (!m_cover.isEmpty()) m_np->setIconUrl(QUrl(m_cover));
    m_np->setMediaState(m_playing ? bb::multimedia::MediaState::Started : bb::multimedia::MediaState::Paused);
}

void Player::onAudioHandle(unsigned int handle)
{
    if (handle) m_np->setAudioManagerHandle(handle);
}

void Player::onNpPlay()
{
    if (m_bgClosed) { // closed in the background: reopen, then resume
        m_bgClosed = false;
        m_resumeOnOpen = true;
        if (configured()) connectNow();
        return;
    }
    if (!m_playing) playPause();
}

void Player::onNpPause()
{
    if (m_playing) playPause();
}

void Player::onNpRevoked()
{
    // Not a reason to pause: the service is revoked also when our own stream
    // starts a track (every song paused itself). Calls and other players
    // already silence the stream through the audio manager; the controls are
    // taken back at the next update.
}
