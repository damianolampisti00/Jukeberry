// jbprobe: end-to-end check of a Jukeberry gateway, acting like the phone.
// Connects, lists playlists, plays a track, receives ~10 s of audio, decodes
// it to a WAV file and reports timing, seek and art results.
//
//	jbprobe -url ws://127.0.0.1:8099/ws -token-file data/token -track spotify:track:... -wav /tmp/probe.wav
package main

/*
#cgo pkg-config: opus
#include <opus.h>
*/
import "C"

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"net/http"
	"os"
	"strings"
	"time"
	"unsafe"

	"github.com/coder/websocket"
)

func main() {
	wsURL := flag.String("url", "ws://127.0.0.1:8099/ws", "gateway WebSocket URL")
	tokenFile := flag.String("token-file", "data/token", "token file")
	track := flag.String("track", "spotify:track:4uLU6hMCjMI75M1A2tKUQC", "track to play")
	wavPath := flag.String("wav", "/tmp/jbprobe.wav", "decoded audio output")
	seconds := flag.Int("seconds", 10, "seconds of audio to receive")
	flag.Parse()

	tok, err := os.ReadFile(*tokenFile)
	if err != nil {
		log.Fatal(err)
	}
	ctx := context.Background()

	// Wrong token first: must be refused with 401.
	if _, resp, err := websocket.Dial(ctx, *wsURL, &websocket.DialOptions{HTTPHeader: http.Header{"Authorization": {"Bearer wrong"}}}); err == nil {
		log.Fatal("wrong token accepted!")
	} else if resp != nil {
		fmt.Printf("wrong token -> HTTP %d (expected 401)\n", resp.StatusCode)
	}

	conn, _, err := websocket.Dial(ctx, *wsURL, &websocket.DialOptions{HTTPHeader: http.Header{"Authorization": {"Bearer " + strings.TrimSpace(string(tok))}}})
	if err != nil {
		log.Fatal(err)
	}
	conn.SetReadLimit(4 << 20)
	defer conn.CloseNow()

	var id int64
	send := func(m map[string]any) int64 {
		id++
		m["id"] = id
		b, _ := json.Marshal(m)
		if err := conn.Write(ctx, websocket.MessageText, b); err != nil {
			log.Fatal(err)
		}
		return id
	}

	var derr C.int
	dec := C.opus_decoder_create(48000, 2, &derr)
	if dec == nil {
		log.Fatal("opus_decoder_create failed")
	}
	pcm := make([]int16, 5760*2)
	var wav []int16

	replies := map[int64]map[string]any{}
	var events []string
	var coverURL string
	frames, discont, lastSeq := 0, 0, int64(-1)
	gaps := 0
	var firstAudio, lastArrival time.Time
	var maxGap time.Duration
	artBytes := 0

	sendStatus := send(map[string]any{"cmd": "status"})
	sendPl := send(map[string]any{"cmd": "playlists", "limit": 3})
	sendSearch := send(map[string]any{"cmd": "search", "q": "vasco rossi", "limit": 2})
	send(map[string]any{"cmd": "audio", "on": true})
	playID := send(map[string]any{"cmd": "play", "uri": *track})
	start := time.Now()
	seekSent, pauseSent, artAsked := false, false, false
	deadline := start.Add(time.Duration(*seconds)*time.Second + 5*time.Second)

	for time.Now().Before(deadline) {
		rctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		typ, msg, err := conn.Read(rctx)
		cancel()
		if err != nil {
			if rctx.Err() != nil {
				continue
			}
			log.Fatal(err)
		}
		if typ == websocket.MessageText {
			var m map[string]any
			json.Unmarshal(msg, &m)
			if ev, ok := m["event"].(string); ok {
				events = append(events, ev)
				if ev == "metadata" {
					if d, ok := m["data"].(map[string]any); ok {
						if u, ok := d["album_cover_url"].(string); ok {
							coverURL = u
						}
					}
				}
				continue
			}
			if rid, ok := m["id"].(float64); ok {
				replies[int64(rid)] = m
			}
			continue
		}
		if len(msg) < 8 {
			continue
		}
		switch msg[0] {
		case 0x02:
			artBytes = len(msg) - 8
			continue
		case 0x01:
		default:
			continue
		}
		now := time.Now()
		if firstAudio.IsZero() {
			firstAudio = now
		} else if g := now.Sub(lastArrival); g > maxGap && frames > 20 {
			maxGap = g
		}
		lastArrival = now
		seq := int64(binary.LittleEndian.Uint32(msg[4:]))
		if lastSeq >= 0 && seq != lastSeq+1 {
			gaps++
		}
		lastSeq = seq
		if msg[1]&1 != 0 {
			discont++
		}
		frames++
		pkt := msg[8:]
		n := C.opus_decode(dec, (*C.uchar)(unsafe.Pointer(&pkt[0])), C.opus_int32(len(pkt)), (*C.opus_int16)(unsafe.Pointer(&pcm[0])), 5760, 0)
		if n > 0 {
			wav = append(wav, pcm[:int(n)*2]...)
		}
		elapsed := now.Sub(firstAudio)
		if !seekSent && elapsed > 4*time.Second {
			send(map[string]any{"cmd": "seek", "position": 60000})
			seekSent = true
		}
		if !artAsked && coverURL != "" {
			send(map[string]any{"cmd": "art", "url": coverURL})
			artAsked = true
		}
		if !pauseSent && elapsed > time.Duration(*seconds)*time.Second {
			send(map[string]any{"cmd": "pause"})
			pauseSent = true
			deadline = now.Add(1500 * time.Millisecond)
		}
	}

	audioSecs := float64(frames) * 0.02
	wall := lastArrival.Sub(firstAudio).Seconds()
	fmt.Printf("status reply ok=%v; playlists ok=%v; play ok=%v\n", replies[sendStatus]["ok"], replies[sendPl]["ok"], replies[playID]["ok"])
	if pl, ok := replies[sendPl]["data"].(map[string]any); ok {
		if items, ok := pl["items"].([]any); ok {
			for _, it := range items {
				p := it.(map[string]any)
				fmt.Printf("  playlist: %v (%v tracks)\n", p["name"], p["length"])
			}
		}
	}
	if res, ok := replies[sendSearch]["data"].(map[string]any); ok {
		for kind, items := range res {
			for _, it := range items.([]any) {
				m := it.(map[string]any)
				fmt.Printf("  search %s: %v | %v | image %v\n", kind, m["name"], m["subtitle"], m["image"] != nil)
			}
		}
	}
	for rid, r := range replies {
		if r["ok"] != true {
			fmt.Printf("reply %d error: %v\n", rid, r["error"])
		}
	}
	fmt.Printf("time to first audio: %.0f ms\n", firstAudio.Sub(start).Seconds()*1000)
	fmt.Printf("frames %d = %.1f s of audio in %.1f s wall; seq gaps %d; discontinuity flags %d; max inter-frame gap %.0f ms\n",
		frames, audioSecs, wall, gaps, discont, maxGap.Seconds()*1000)
	fmt.Printf("events: %s\n", strings.Join(events, ","))
	fmt.Printf("cover art: %d bytes\n", artBytes)

	var sum float64
	for _, s := range wav {
		sum += float64(s) * float64(s)
	}
	if len(wav) > 0 {
		fmt.Printf("decoded RMS: %.1f dBFS\n", 20*math.Log10(math.Sqrt(sum/float64(len(wav)))/32768))
	}
	writeWAV(*wavPath, wav)
	fmt.Printf("wrote %s\n", *wavPath)
}

func writeWAV(path string, s []int16) {
	f, err := os.Create(path)
	if err != nil {
		log.Fatal(err)
	}
	defer f.Close()
	data := len(s) * 2
	h := make([]byte, 44)
	copy(h, "RIFF")
	binary.LittleEndian.PutUint32(h[4:], uint32(36+data))
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 2)
	binary.LittleEndian.PutUint32(h[24:], 48000)
	binary.LittleEndian.PutUint32(h[28:], 48000*4)
	binary.LittleEndian.PutUint16(h[32:], 4)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], uint32(data))
	f.Write(h)
	binary.Write(f, binary.LittleEndian, s)
}
