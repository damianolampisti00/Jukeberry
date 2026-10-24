#ifndef PLAYER_HPP_
#define PLAYER_HPP_

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <bb/cascades/ArrayDataModel>

#include "audioring.hpp"

class AudioOutput;
class Link;

namespace bb { namespace multimedia { class NowPlayingConnection; } }

// The app's state and actions, for QML (context property "player").
// State comes from the gateway: the status at connect, then go-librespot's
// events (metadata, playing, paused, seek...). Actions are JSON commands
// answered by id (see server/gateway.go).
class Player : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY settingsChanged)
    Q_PROPERTY(QString token READ token NOTIFY settingsChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY settingsChanged)

    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY trackChanged)
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString trackUri READ trackUri NOTIFY trackChanged)
    Q_PROPERTY(QString artists READ artists NOTIFY trackChanged)
    Q_PROPERTY(QString album READ album NOTIFY trackChanged)
    Q_PROPERTY(QString contextName READ contextName NOTIFY trackChanged)
    Q_PROPERTY(QString cover READ cover NOTIFY coverChanged)
    Q_PROPERTY(int duration READ duration NOTIFY trackChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playStateChanged)
    Q_PROPERTY(bool liked READ liked NOTIFY likedChanged)
    Q_PROPERTY(bool shuffle READ shuffle NOTIFY modesChanged)
    Q_PROPERTY(int repeat READ repeat NOTIFY modesChanged) // 0 off, 1 context, 2 track

    Q_PROPERTY(bb::cascades::DataModel *playlists READ playlists CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel *tracks READ tracks CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel *results READ results CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel *queue READ queue CONSTANT)
    Q_PROPERTY(bool queueLoading READ queueLoading NOTIFY queueChanged)
    Q_PROPERTY(QString tracksTitle READ tracksTitle NOTIFY tracksChanged)
    Q_PROPERTY(QString tracksUri READ tracksUri NOTIFY tracksChanged)
    Q_PROPERTY(bool tracksLoading READ tracksLoading NOTIFY tracksChanged)
    Q_PROPERTY(bool playlistsLoading READ playlistsLoading NOTIFY playlistsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY resultsChanged)

public:
    explicit Player(QObject *parent = 0);
    ~Player();

    bool connected() const { return m_connected; }
    QString message() const { return m_message; }
    QString serverUrl() const { return m_serverUrl; }
    QString token() const { return m_token; }
    bool configured() const { return !m_serverUrl.isEmpty() && !m_token.isEmpty(); }

    bool hasTrack() const { return !m_trackUri.isEmpty(); }
    QString trackUri() const { return m_trackUri; }
    QString title() const { return m_title; }
    QString artists() const { return m_artists; }
    QString album() const { return m_album; }
    QString contextName() const { return m_contextName; }
    QString cover() const { return m_cover; }
    int duration() const { return m_duration; }
    int position() const;
    bool playing() const { return m_playing; }
    bool liked() const { return m_liked; }
    bool shuffle() const { return m_shuffle; }
    int repeat() const { return m_repeat; }

    bb::cascades::DataModel *playlists() const { return m_playlists; }
    bb::cascades::DataModel *tracks() const { return m_tracks; }
    bb::cascades::DataModel *results() const { return m_results; }
    bb::cascades::DataModel *queue() const { return m_queue; }
    bool queueLoading() const { return m_queueLoading; }
    QString tracksTitle() const { return m_tracksTitle; }
    QString tracksUri() const { return m_tracksUri; }
    bool tracksLoading() const { return m_tracksLoading; }
    bool playlistsLoading() const { return m_playlistsLoading; }
    bool searching() const { return m_searching; }

    Q_INVOKABLE void saveSettings(const QString &serverUrl, const QString &token);
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void seekTo(int ms);
    Q_INVOKABLE void toggleShuffle();
    Q_INVOKABLE void cycleRepeat();
    Q_INVOKABLE void toggleLike();
    // Plays `uri` (a track, or a playlist/album/artist), starting at `skipTo` if given.
    Q_INVOKABLE void play(const QString &uri, const QString &skipTo = QString());
    Q_INVOKABLE void loadPlaylists();
    Q_INVOKABLE void openContext(const QString &uri, const QString &title);
    Q_INVOKABLE void search(const QString &query);
    // The play queue (upcoming tracks in play order): refreshed by
    // loadQueue() and after every track change while it is shown.
    Q_INVOKABLE void setQueueVisible(bool visible);
    // Jumps ahead to an upcoming track; adds a track to the queue.
    Q_INVOKABLE void skipTo(const QString &uri);
    Q_INVOKABLE void addToQueue(const QString &uri);
    // Library: Liked Songs and the user's own playlists.
    Q_INVOKABLE void setLiked(const QString &uri, bool liked);
    Q_INVOKABLE QVariantList editablePlaylists() const; // [{name, uri}], the ones the user may add to
    Q_INVOKABLE void addToPlaylist(const QString &playlistUri, const QString &playlistName,
                                   const QString &trackUri);
    // The tracks added to the queue (not the context's): uid from the queue model.
    Q_INVOKABLE void removeFromQueue(const QString &uid);
    Q_INVOKABLE void moveInQueue(const QString &uid, int delta); // -1 up, +1 down
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE QString formatTime(int ms) const;

public slots:
    void loadQueue();
    // Slots, not just invokables: the lock screen's next/previous connect to them.
    void next();
    void previous();

signals:
    void connectedChanged();
    void messageChanged();
    void settingsChanged();
    void trackChanged();
    void coverChanged();
    void positionChanged();
    void playStateChanged();
    void likedChanged();
    void modesChanged();
    void playlistsChanged();
    void tracksChanged();
    void resultsChanged();
    void queueChanged();
    void queued();
    // A short confirmation for a toast ("Added to <playlist>"...).
    void notice(const QString &text);

    // To the link thread.
    void linkConnect(const QUrl &url, const QByteArray &token);
    void linkSend(const QByteArray &utf8);
    void linkDisconnect();
    void linkPlaying(bool playing);
    void linkSkipTo(quint32 seq);

private slots:
    void onDiscontinuity();
    void applyPendingTrack();
    void onOpened();
    void onClosed(const QString &reason, bool authFailed, bool lockedOut);
    void onText(const QByteArray &utf8);
    void onImage(quint32 requestId, const QByteArray &image);
    void onTick();
    void pollContext();
    // Lock screen / headset controls.
    void onNpPlay();
    void onNpPause();
    void onNpRevoked();
    void onAudioHandle(unsigned int handle);
    // Minimized/covered and not playing: the connection is closed after a
    // while; back in full screen (or play from the lock screen) it reopens.
    void onAppBackground();
    void onAppForeground();
    void onBackgroundIdle();

private:
    enum Purpose { None, Status, Playlists, Context, Search, Liked, Art, Queue, LikedList, PlaylistAdd };
    struct Pending {
        Pending() : purpose(None) {}
        Purpose purpose;
        QString key;
    };

    void requestLiked(bb::cascades::ArrayDataModel *model);
    void applyLikedCache();
    QStringList queuedUids() const;
    void sendQueue(const QStringList &uids);
    qint64 send(const QString &cmd, const QVariantMap &args = QVariantMap(), Purpose purpose = None,
                const QString &key = QString());
    void connectNow();
    void setMessage(const QString &m);
    void applyStatus(const QVariantMap &status);
    void applyTrack(const QVariantMap &track);
    void onEvent(const QString &type, const QVariantMap &data);
    void setPlaying(bool playing);
    void setPositionBase(qint64 ms);
    // ms between what the gateway has sent and what is audible (our queue)
    qint64 lagMs() const;
    void applyMetadata(const QVariantMap &track, qint64 lagMs);
    void fillContext(const QVariantMap &data);
    void fillResults(const QVariantMap &data);
    void fillQueue(const QVariantMap &data);
    // Cover art: a local file per URL; "" until it is fetched.
    QString artFile(const QString &url) const;
    void requestArt(const QString &url);
    void applyArt(const QString &url, const QString &file);
    void updateNowPlaying();

    QString m_serverUrl, m_token;
    bool m_connected;
    QString m_message;

    AudioRing m_ring;
    QThread m_linkThread;
    Link *m_link;
    AudioOutput *m_audio;

    qint64 m_nextId;
    QHash<qint64, Pending> m_pending;
    QHash<QString, bool> m_artInFlight;
    QHash<QString, bool> m_likedCache; // track uri -> in Liked Songs
    QString m_artDir;

    QString m_trackUri, m_title, m_artists, m_album, m_contextName, m_coverUrl, m_cover;
    int m_duration;
    qint64 m_posBase;
    QElapsedTimer m_posClock;
    bool m_playing, m_liked, m_shuffle;
    int m_repeat;
    QTimer m_tick;
    QTimer m_bgTimer;
    bool m_background;
    bool m_bgClosed;     // the connection was closed on purpose in the background
    bool m_resumeOnOpen; // play pressed (lock screen, headset) while it was closed
    // The next track's metadata, which arrives as soon as the gateway reaches
    // it (up to 30 s before we play it): shown when the audio gets there.
    QVariantMap m_pendingTrack;
    bool m_hasPending;
    QTimer m_pendingTimer;

    bb::cascades::ArrayDataModel *m_playlists, *m_tracks, *m_results, *m_queue;
    bool m_queueLoading, m_queueVisible;
    QString m_tracksTitle, m_tracksUri;
    bool m_tracksLoading, m_playlistsLoading, m_searching;
    int m_contextPolls;
    QTimer m_contextTimer;

    bb::multimedia::NowPlayingConnection *m_np;
};

#endif /* PLAYER_HPP_ */
