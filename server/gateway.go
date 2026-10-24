package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

type gateway struct {
	guard *guard
	ls    *librespot
	audio *audioHub
	web   *webAPI
	art   *artCache

	mu      sync.Mutex
	clients map[*client]struct{}
}

type outMsg struct {
	binary bool
	data   []byte
}

type client struct {
	ip      string
	ctrl    chan outMsg // replies, events, images: never dropped
	audio   chan []byte // Opus frames: dropped when the phone falls behind
	done    chan struct{}
	closing sync.Once
	dropped atomic.Uint64
	busy    chan struct{} // bounds the commands running at once
}

func (c *client) close() { c.closing.Do(func() { close(c.done) }) }

func (c *client) sendAudio(frame []byte) {
	select {
	case c.audio <- frame:
	default:
		c.dropped.Add(1)
	}
}

func (c *client) send(m outMsg) {
	select {
	case c.ctrl <- m:
	case <-c.done:
	default:
		// A phone that stopped reading: let it reconnect.
		log.Printf("ws %s: control queue full, closing", c.ip)
		c.close()
	}
}

func (c *client) sendJSON(v any) {
	b, err := json.Marshal(v)
	if err != nil {
		log.Printf("ws: marshal: %v", err)
		return
	}
	c.send(outMsg{data: b})
}

func (g *gateway) serveWS(w http.ResponseWriter, r *http.Request) {
	ip := clientIP(r)
	if ok, wait := g.guard.check(ip, bearer(r)); !ok {
		if wait > 0 {
			w.Header().Set("Retry-After", fmt.Sprint(int(wait.Seconds())+1))
			http.Error(w, "too many attempts", http.StatusTooManyRequests)
		} else {
			http.Error(w, "missing or invalid token", http.StatusUnauthorized)
		}
		return
	}
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{CompressionMode: websocket.CompressionDisabled})
	if err != nil {
		return
	}
	conn.SetReadLimit(64 << 10)
	c := &client{
		ip:    ip,
		ctrl:  make(chan outMsg, 256),
		audio: make(chan []byte, lead+500), // 40 s: the margin of a phone that acks, and a stall's worth
		done:  make(chan struct{}),
		busy:  make(chan struct{}, 8),
	}
	g.mu.Lock()
	g.clients[c] = struct{}{}
	g.mu.Unlock()
	log.Printf("ws %s: connected", ip)

	ctx, cancel := context.WithCancel(r.Context())
	defer func() {
		cancel()
		c.close()
		g.audio.subscribe(c, false) // also forgets its acks
		g.mu.Lock()
		delete(g.clients, c)
		g.mu.Unlock()
		conn.CloseNow()
		log.Printf("ws %s: disconnected (audio frames dropped: %d)", ip, c.dropped.Load())
	}()

	go g.writer(ctx, conn, c)
	go func() { // keepalive: Cloudflare closes idle WebSockets after 100 s
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-t.C:
				pctx, pcancel := context.WithTimeout(ctx, 15*time.Second)
				err := conn.Ping(pctx)
				pcancel()
				if err != nil {
					c.close()
					return
				}
			case <-c.done:
				return
			}
		}
	}()

	for {
		select {
		case <-c.done:
			return
		default:
		}
		typ, msg, err := conn.Read(ctx)
		if err != nil {
			return
		}
		if typ != websocket.MessageText {
			continue
		}
		var cmd command
		if err := json.Unmarshal(msg, &cmd); err != nil {
			c.sendJSON(reply{OK: false, Error: "bad JSON"})
			continue
		}
		if cmd.Cmd == "audio_ack" { // {seq}: every second while listening; no reply
			if cmd.Seq != nil {
				g.audio.ack(c, *cmd.Seq)
			}
			continue
		}
		select {
		case c.busy <- struct{}{}:
		case <-c.done:
			return
		}
		go func() {
			defer func() { <-c.busy }()
			cctx, ccancel := context.WithTimeout(ctx, 30*time.Second)
			defer ccancel()
			g.handleCommand(cctx, c, &cmd)
		}()
	}
}

func (g *gateway) writer(ctx context.Context, conn *websocket.Conn, c *client) {
	defer c.close()
	write := func(typ websocket.MessageType, b []byte) bool {
		wctx, cancel := context.WithTimeout(ctx, 15*time.Second)
		defer cancel()
		return conn.Write(wctx, typ, b) == nil
	}
	for {
		// Control messages first, so a reply never waits behind audio.
		select {
		case m := <-c.ctrl:
			if !write(msgType(m), m.data) {
				return
			}
			continue
		default:
		}
		select {
		case m := <-c.ctrl:
			if !write(msgType(m), m.data) {
				return
			}
		case f := <-c.audio:
			if !write(websocket.MessageBinary, f) {
				return
			}
		case <-c.done:
			return
		}
	}
}

func msgType(m outMsg) websocket.MessageType {
	if m.binary {
		return websocket.MessageBinary
	}
	return websocket.MessageText
}

// onPlayerEvent relays go-librespot's events to every phone.
func (g *gateway) onPlayerEvent(ev playerEvent) {
	var pos struct {
		Position int64 `json:"position"`
		Duration int64 `json:"duration"`
	}
	switch ev.Type {
	case "seek":
		g.audio.flush.Store(true)
		if json.Unmarshal(ev.Data, &pos) == nil {
			g.audio.sought(pos.Position)
		}
	case "metadata": // a new track: natural (at the old one's end) or a jump
		if json.Unmarshal(ev.Data, &pos) == nil {
			g.audio.trackStarted(pos.Position, pos.Duration)
		}
	}
	switch ev.Type {
	case "paused", "playing", "stopped", "not_playing", "inactive", "active":
		// Who stops the music: a "pause" command from a phone is logged
		// with it; a pause without one came from Spotify (another device).
		log.Printf("librespot: %s %s", ev.Type, ev.Data)
	}
	var data any // omitted, as before, when the event has none
	if len(ev.Data) > 0 {
		data = ev.Data
	}
	g.broadcastEvent(ev.Type, data)
}

// broadcastEvent: {"event", "data"} to every phone (go-librespot's events,
// and the gateway's own: skip_to {seq}).
func (g *gateway) broadcastEvent(event string, data any) {
	b, err := json.Marshal(struct {
		Event string `json:"event"`
		Data  any    `json:"data,omitempty"`
	}{event, data})
	if err != nil {
		return
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	for c := range g.clients {
		c.send(outMsg{data: b})
	}
}

type command struct {
	ID        int64    `json:"id"`
	Cmd       string   `json:"cmd"`
	URI       string   `json:"uri"`
	SkipToURI string   `json:"skip_to_uri"`
	Position  *int64   `json:"position"`
	Paused    bool     `json:"paused"`
	Relative  bool     `json:"relative"`
	Volume    *int     `json:"volume"`
	Value     *bool    `json:"value"`
	On        *bool    `json:"on"`
	URIs      []string `json:"uris"`
	UIDs      []string `json:"uids"`
	Liked     *bool    `json:"liked"`
	Offset    int      `json:"offset"`
	Limit     int      `json:"limit"`
	Q         string   `json:"q"`
	Types     string   `json:"types"`
	URL       string   `json:"url"`
	Seq       *uint32  `json:"seq"`
	From      *uint32  `json:"from"`
}

type reply struct {
	ID    int64           `json:"id"`
	OK    bool            `json:"ok"`
	Data  json.RawMessage `json:"data,omitempty"`
	Error string          `json:"error,omitempty"`
}

var spotifyURI = regexp.MustCompile(`^spotify:[A-Za-z0-9:_.-]{1,200}$`)

func (g *gateway) handleCommand(ctx context.Context, c *client, cmd *command) {
	data, err := g.run(ctx, c, cmd)
	if errors.Is(err, errHandled) {
		return
	}
	r := reply{ID: cmd.ID, OK: err == nil, Data: data}
	if err != nil {
		r.Error = err.Error()
	}
	c.sendJSON(r)
}

// errHandled: the command already answered (the image reply is binary).
var errHandled = errors.New("handled")

func (g *gateway) run(ctx context.Context, c *client, cmd *command) (json.RawMessage, error) {
	needURI := func(u string) error {
		if !spotifyURI.MatchString(u) {
			return fmt.Errorf("invalid uri %q", truncate(u, 60))
		}
		return nil
	}
	// call: one go-librespot request; flush marks the audio after a jump.
	call := func(method, path string, body any, flush bool) (json.RawMessage, error) {
		defer g.audio.command()() // go-librespot needs us reading to run it
		status, data, err := g.ls.call(ctx, method, path, body)
		if err != nil {
			return nil, err
		}
		switch {
		case status == http.StatusNoContent && data == nil && method == "GET":
			return nil, errors.New("no Spotify session on the server")
		case status >= 400:
			return nil, fmt.Errorf("player: HTTP %d", status)
		}
		if flush {
			g.audio.flush.Store(true)
		}
		return data, nil
	}

	switch cmd.Cmd {
	case "ping":
		return json.RawMessage(`"pong"`), nil
	case "status":
		return call("GET", "/status", nil, false)
	case "audio": // {on, from?}: stream this phone the audio (from: the first frame it lacks), or stop
		on := cmd.On != nil && *cmd.On
		g.audio.subscribeFrom(c, on, cmd.From)
		log.Printf("ws %s: audio %v", c.ip, on)
		return nil, nil
	case "play": // {uri, skip_to_uri?, position?, paused?}
		if err := needURI(cmd.URI); err != nil {
			return nil, err
		}
		body := map[string]any{"uri": cmd.URI, "paused": cmd.Paused}
		if cmd.SkipToURI != "" {
			if err := needURI(cmd.SkipToURI); err != nil {
				return nil, err
			}
			body["skip_to_uri"] = cmd.SkipToURI
		}
		if cmd.Position != nil && *cmd.Position > 0 {
			body["position"] = *cmd.Position
		}
		return call("POST", "/player/play", body, true)
	case "pause", "resume", "playpause", "stop":
		log.Printf("ws %s: %s", c.ip, cmd.Cmd)
		return call("POST", "/player/"+cmd.Cmd, nil, false)
	case "prev":
		return call("POST", "/player/prev", nil, true)
	case "next": // {uri?}
		var body any
		if cmd.URI != "" {
			if err := needURI(cmd.URI); err != nil {
				return nil, err
			}
			body = map[string]string{"uri": cmd.URI}
		} else if at, held := g.audio.held(); held {
			// go-librespot already plays the next track (our margin reached
			// it) while the phones still play the one before: its "next"
			// would skip one. The phones jump to the queued start instead.
			g.audio.release()
			g.broadcastEvent("skip_to", map[string]uint32{"seq": at})
			log.Printf("ws %s: next within the margin (skip_to %d)", c.ip, at)
			return nil, nil
		}
		return call("POST", "/player/next", body, true)
	case "seek": // {position, relative?}
		if cmd.Position == nil {
			return nil, errors.New("position is required")
		}
		if _, held := g.audio.held(); held && !cmd.Relative {
			// The phones still play the track before the one go-librespot
			// reads: back to it first, then the seek is in it.
			if _, err := call("POST", "/player/prev", nil, true); err != nil {
				return nil, err
			}
		}
		return call("POST", "/player/seek", map[string]any{"position": *cmd.Position, "relative": cmd.Relative}, true)
	case "volume": // {volume, relative?}; without volume: read it
		if cmd.Volume == nil {
			return call("GET", "/player/volume", nil, false)
		}
		return call("POST", "/player/volume", map[string]any{"volume": *cmd.Volume, "relative": cmd.Relative}, false)
	case "shuffle", "repeat_context", "repeat_track": // {value}
		if cmd.Value == nil {
			return nil, errors.New("value is required")
		}
		path, key := "/player/"+cmd.Cmd, cmd.Cmd
		if cmd.Cmd == "shuffle" {
			path, key = "/player/shuffle_context", "shuffle_context"
		}
		return call("POST", path, map[string]bool{key: *cmd.Value}, false)
	case "queue": // {uri}
		if err := needURI(cmd.URI); err != nil {
			return nil, err
		}
		return call("POST", "/player/add_to_queue", map[string]string{"uri": cmd.URI}, false)
	case "upcoming": // the queue: upcoming tracks in play order (patched go-librespot, GET /player/queue)
		return call("GET", "/player/queue", nil, false)
	case "setqueue": // {uids}: the queued tracks to keep, in order (patched go-librespot, POST /player/queue/set)
		if cmd.UIDs == nil {
			cmd.UIDs = []string{}
		}
		if len(cmd.UIDs) > 500 {
			return nil, errors.New("too many uids")
		}
		return call("POST", "/player/queue/set", map[string]any{"uids": cmd.UIDs}, false)
	case "playlistadd": // {uri: playlist, uris: tracks}: append to a playlist the user may edit
		if !strings.HasPrefix(cmd.URI, "spotify:playlist:") {
			return nil, errors.New("uri must be a playlist")
		}
		if len(cmd.URIs) == 0 || len(cmd.URIs) > 50 {
			return nil, errors.New("1 to 50 uris")
		}
		for _, u := range cmd.URIs {
			if err := needURI(u); err != nil {
				return nil, err
			}
		}
		return call("POST", "/library/playlists/add_tracks", map[string]any{"playlist_uri": cmd.URI, "uris": cmd.URIs}, false)
	case "playlists": // {offset?, limit?}
		limit := cmd.Limit
		if limit <= 0 || limit > 500 {
			limit = 50
		}
		v := url.Values{"offset": {fmt.Sprint(max(cmd.Offset, 0))}, "limit": {fmt.Sprint(limit)}}
		return call("GET", "/library/playlists?"+v.Encode(), nil, false)
	case "context": // {uri}: a playlist's, album's, artist's tracks (poll until ready)
		if err := needURI(cmd.URI); err != nil {
			return nil, err
		}
		return call("GET", "/context/tracks?"+url.Values{"uri": {cmd.URI}}.Encode(), nil, false)
	case "liked": // {uris}: are they in Liked Songs?
		if len(cmd.URIs) == 0 || len(cmd.URIs) > 50 {
			return nil, errors.New("1 to 50 uris")
		}
		for _, u := range cmd.URIs {
			if err := needURI(u); err != nil {
				return nil, err
			}
		}
		return call("GET", "/library/liked?"+url.Values{"uris": {strings.Join(cmd.URIs, ",")}}.Encode(), nil, false)
	case "like": // {uris, liked}
		if len(cmd.URIs) == 0 || len(cmd.URIs) > 50 || cmd.Liked == nil {
			return nil, errors.New("uris (1 to 50) and liked are required")
		}
		for _, u := range cmd.URIs {
			if err := needURI(u); err != nil {
				return nil, err
			}
		}
		return call("POST", "/library/liked", map[string]any{"uris": cmd.URIs, "liked": *cmd.Liked}, false)
	case "search": // {q, types?, limit?, offset?}
		q := strings.TrimSpace(cmd.Q)
		if q == "" || len(q) > 200 {
			return nil, errors.New("q is required (max 200 characters)")
		}
		types := cmd.Types
		if types == "" {
			types = "track,album,artist,playlist"
		}
		for _, t := range strings.Split(types, ",") {
			switch t {
			case "track", "album", "artist", "playlist":
			default:
				return nil, fmt.Errorf("unknown type %q", truncate(t, 20))
			}
		}
		limit := cmd.Limit
		if limit <= 0 || limit > 10 {
			limit = 10 // the Web API's maximum since February 2026
		}
		res, err := g.web.search(ctx, q, types, limit, max(cmd.Offset, 0))
		if err != nil {
			return nil, err
		}
		b, err := json.Marshal(res)
		return b, err
	case "art": // {url} -> binary [0x02][0][0][0][id u32 LE][image]
		img, err := g.art.get(ctx, cmd.URL)
		if err != nil {
			return nil, err
		}
		frame := make([]byte, 8+len(img))
		frame[0] = 0x02
		binary.LittleEndian.PutUint32(frame[4:], uint32(cmd.ID))
		copy(frame[8:], img)
		c.send(outMsg{binary: true, data: frame})
		return nil, errHandled
	}
	return nil, fmt.Errorf("unknown command %q", truncate(cmd.Cmd, 30))
}
