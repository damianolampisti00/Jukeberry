#include "player.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>

#include <bb/data/JsonDataAccess>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/MetaData>
#include <bb/multimedia/NowPlayingConnection>
#include <bb/multimedia/OverlayStyle>

#include "audiooutput.hpp"
#include "link.hpp"

using namespace bb::cascades;
using bb::multimedia::NowPlayingConnection;

namespace {

const int kRingBytes = AudioOutput::kBytesPerMs * 2000; // up to 2 s queued
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
        m_np(0)
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

    m_tick.setInterval(500);
    connect(&m_tick, SIGNAL(timeout()), this, SLOT(onTick()));
    m_tick.start();
    m_contextTimer.setSingleShot(true);
    connect(&m_contextTimer, SIGNAL(timeout()), this, SLOT(pollContext()));

    // The network on its own thread, the audio on another.
    m_link = new Link(&m_ring);
    m_link->moveToThread(&m_linkThread);
    connect(this, SIGNAL(linkConnect(QUrl, QByteArray)), m_link, SLOT(connectTo(QUrl, QByteArray)));
    connect(this, SIGNAL(linkSend(QByteArray)), m_link, SLOT(sendText(QByteArray)));
    connect(m_link, SIGNAL(opened()), this, SLOT(onOpened()));
    connect(m_link, SIGNAL(closed(QString, bool, bool)), this, SLOT(onClosed(QString, bool, bool)));
    connect(m_link, SIGNAL(textReceived(QByteArray)), this, SLOT(onText(QByteArray)));
    connect(m_link, SIGNAL(imageReceived(quint32, QByteArray)), this, SLOT(onImage(quint32, QByteArray)));
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
    if (authFailed) setMessage(tr("Invalid token: check Settings"));
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
        applyTrack(data);
        setPositionBase(data.value("position").toLongLong());
        if (m_queueVisible) loadQueue(); // the queue moved on
    } else if (type == "playing") {
        setPlaying(true);
    } else if (type == "paused" || type == "stopped") {
        trace("event " + type.toUtf8() + " " + data.value("play_origin").toString().toUtf8());
        setPlaying(false);
    } else if (type == "seek") {
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
        applyTrack(track);
        setPositionBase(track.value("position").toLongLong());
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

int Player::position() const
{
    qint64 ms = m_posBase;
    if (m_playing && m_posClock.isValid()) {
        // go-librespot counts what it has handed out; what is audible is
        // that minus what still waits in our ring and the device.
        ms += m_posClock.elapsed() - m_ring.level() / AudioOutput::kBytesPerMs - kDeviceLatencyMs;
    }
    if (ms < 0) ms = 0;
    if (m_duration > 0 && ms > m_duration) ms = m_duration;
    return int(ms);
}

void Player::setPositionBase(qint64 ms)
{
    m_posBase = ms;
    m_posClock.restart();
    emit positionChanged();
}

void Player::setPlaying(bool playing)
{
    if (playing == m_playing) return;
    if (playing) {
        m_posClock.restart();
    } else if (m_posClock.isValid()) {
        m_posBase += m_posClock.elapsed();
    }
    m_playing = playing;
    emit playStateChanged();
    emit positionChanged();
    updateNowPlaying();
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
    trace(m_playing ? "pause" : "resume");
    send(m_playing ? "pause" : "resume");
    setPlaying(!m_playing); // at once; the event confirms it
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
            m_results->append(item);
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

// Pauses nobody asked for: a short log of who pauses and resumes, in the
// app's data folder (trace.log, the last ~64 KB), to read over SSH.
void Player::trace(const QByteArray &what)
{
    QFile f(QDir::homePath() + "/trace.log");
    if (f.size() > 64 * 1024) f.remove();
    if (!f.open(QIODevice::Append)) return;
    f.write(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz ").toUtf8() + what
            + (m_playing ? " (playing)" : " (paused)")
            + (m_np->isAcquired() ? " np" : "") + "\n");
}

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
    trace("np play");
    if (!m_playing) playPause();
}

void Player::onNpPause()
{
    trace("np pause");
    if (m_playing) playPause();
}

void Player::onNpRevoked()
{
    trace("np revoked");
    // Another app took the media controls (a video, another player): pause.
    if (m_playing) playPause();
}
