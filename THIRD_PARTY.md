# Third-party components

Jukeberry is licensed under the GNU General Public License v3.0 (`LICENSE`).

| Component | Where | Used for | License |
|---|---|---|---|
| [go-librespot](https://github.com/devgianlu/go-librespot) | separate program on the server, talked to over HTTP | Spotify Connect device, playback, library | GPL-3.0 |
| [libopus](https://opus-codec.org/) | server (system library) and `third_party/opus` (prebuilt armv7) | Encoding and decoding the audio | BSD-3-Clause (`third_party/opus/COPYING`) |
| [libsoxr](https://sourceforge.net/projects/soxr/) | server (system library) | Resampling 44.1 kHz to 48 kHz | LGPL-2.1 |
| [coder/websocket](https://github.com/coder/websocket) | server, Go module | WebSocket server and client | ISC |
| [Material Symbols](https://fonts.google.com/icons) by Google | `client/assets/images` (rendered PNGs) | Icons | Apache-2.0 (`client/assets/images/LICENSE-material-symbols.txt`) |
| BlackBerry 10 Native SDK / Cascades | build toolchain, system libraries on the phone | UI framework and platform APIs | BlackBerry SDK license; linked as system libraries |

The WebSocket client in `client/src/websocketclient.*` comes from the author's
[Berry Bridge fork](https://github.com/damianolampisti00/BerryBridge).

Spotify is a trademark of Spotify AB. Jukeberry is not affiliated with Spotify.
