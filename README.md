# Jukeberry

A Spotify Connect player for BlackBerry 10 phones. Your own server runs
[go-librespot](https://github.com/devgianlu/go-librespot) as a Spotify Connect device;
a small gateway streams its sound to the phone as Opus and passes the controls, your
library and search back and forth. The phone app is a native Cascades app.

> **Not affiliated with, endorsed by or connected to Spotify.** Jukeberry needs a
> **Spotify Premium** account and plays only what Spotify lets that account play. It never
> downloads or stores music. Using unofficial clients is against Spotify's terms of use:
> use it at your own risk, with your own account.

Tested on a BlackBerry Q5 (BB10 10.3.3) with the server on Linux.

```
 BlackBerry 10                                   your server
 +---------------------------+   WebSocket   +-----------------------------------+
 | Jukeberry app             |   over TLS    | jukeberry-gateway (server/, Go)   |
 |  Now Playing, Library,    | <-----------> |  token, Opus 160 kbit/s, controls |
 |  Search, lock screen      |  Opus + JSON  |  cover art, Web API search        |
 |  libopus -> libasound     |               | go-librespot (Connect device)     |
 +---------------------------+               +-----------------------------------+
```

## Features

- Now Playing with cover art, seek bar, play/pause, next/previous, shuffle, repeat, like,
  and the play queue (tap a track to jump to it; "Add to queue" on any track).
- Your library: Liked Songs and every playlist, albums and artists as track lists.
- Search: songs, albums, artists, playlists.
- Keeps playing in the background and with the screen locked; lock-screen and headset
  controls; an Active Frame with the cover.
- Bluetooth headphones and speakers: the sound follows the output when a device connects
  or disconnects.
- The phone is a Spotify Connect device: you can also start music from Spotify on a
  computer or another phone and hear it on the BlackBerry.
- English and Italian UI.

## Server

Requirements: Linux, Go 1.25 or newer, and the development files of libopus, libsoxr,
libogg, libvorbis, flac, mpg123 and ALSA (Debian/Ubuntu: `libopus-dev libsoxr-dev libogg-dev
libvorbis-dev libflac-dev libmpg123-dev libasound2-dev`; Arch: `opus libsoxr libogg libvorbis
flac mpg123 alsa-lib`).

### 1. go-librespot

```
git clone https://github.com/devgianlu/go-librespot && cd go-librespot
# Optional, for the app's play queue: adds GET /player/queue (tested on v0.10.3).
git apply /path/to/Jukeberry/server/patches/go-librespot-player-queue.patch
go run github.com/oapi-codegen/oapi-codegen/v2/cmd/oapi-codegen@v2.5.0 --config=api-codegen.yml api-spec.yml
go build -o ~/jukeberry/go-librespot-daemon ./cmd/daemon
mkdir -p ~/jukeberry/librespot && mkfifo -m 600 ~/jukeberry/librespot/audio.fifo
```

Without the patch everything works except the Queue screen, which stays empty.

`~/jukeberry/librespot/config.yml`:

```yaml
device_name: "BlackBerry"
device_type: smartphone
zeroconf_enabled: false
credentials:
  type: device_auth          # prints a code to enter at spotify.com/pair
bitrate: 320
audio_backend: pipe
audio_output_pipe: /home/you/jukeberry/librespot/audio.fifo
audio_output_pipe_format: s16le
audio_output_pipe_wait_for_reader: true
server:
  enabled: true
  address: localhost
  port: 3678
metadata:
  enabled: true
```

Start it with `go-librespot-daemon --config_dir ~/jukeberry/librespot`, open the
`spotify.com/pair` link it prints and approve the device. The login is saved.

### 2. The gateway

```
cd server
go build -o ~/jukeberry/jukeberry-gateway .
~/jukeberry/jukeberry-gateway -addr 127.0.0.1:8099 -data ~/jukeberry/data \
    -fifo ~/jukeberry/librespot/audio.fifo
```

On first start it creates an access token in `data/token` (mode 600): the phone needs it.
`go build -o jbprobe ./cmd/jbprobe` builds an end-to-end check that plays a track, decodes
the audio and reports timing.

**Search** uses Spotify's public Web API with your own developer app (go-librespot's
session token is shared by every installation and always rate-limited). Create an app at
[developer.spotify.com/dashboard](https://developer.spotify.com/dashboard) (Web API; any
redirect URI) and put its keys in `data/webapi.json`:

```json
{"client_id": "...", "client_secret": "...", "market": "US"}
```

Without it everything works except search.

### 3. Reaching it from the phone

The gateway listens on 127.0.0.1. Put it behind HTTPS, for example a Cloudflare Tunnel
(`service: http://localhost:8099`; the http2 protocol behaves better with long streams
than QUIC), and use `https://your-host` as the server address in the app.

pm2 or systemd keep both processes running; give pm2 the arguments as an array
(`args: ['--config_dir', '/path']`).

## Phone app

Build on Windows with the BlackBerry 10 Native SDK 10.3 in `C:\bbndk`:

```
powershell -File client\package.ps1
```

BlackBerry's signing service is gone, so the `.bar` is packaged in development mode.
The first installation needs a sideloading tool such as DBBT (Desktop BlackBerry Tool) over
USB with Development Mode on; on a rooted phone `package.ps1 -Install` installs updates
over SSH (copy `client/package.config.example.ps1` to `client/package.config.ps1`).

In the app, open Settings (swipe down from the top) and enter the server address and the
token.

## Protocol

One WebSocket, `GET /ws` with `Authorization: Bearer <token>`. Text messages are JSON
commands and replies (`{"id":1,"cmd":"play","uri":"spotify:..."}` →
`{"id":1,"ok":true}`) plus go-librespot's player events (`{"event":"metadata",...}`).
Binary messages are audio (`0x01`, flags, sequence, one 20 ms Opus packet at 48 kHz stereo)
and cover art (`0x02`, request id, image). The full list is at the top of
`server/main.go` and in `server/gateway.go`.

## License

GPL-3.0, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY.md](THIRD_PARTY.md).
