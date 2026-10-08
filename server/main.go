// Jukeberry gateway: lets a BlackBerry 10 phone play and browse Spotify
// through a go-librespot daemon running on the same machine.
//
// One authenticated WebSocket per phone (GET /ws, Authorization: Bearer):
//
//	phone -> gateway  text   {"id": 1, "cmd": "...", ...args}
//	gateway -> phone  text   {"id": 1, "ok": true, "data": ...}  (the reply)
//	                         {"id": 1, "ok": false, "error": "..."}
//	                         {"event": "<type>", "data": ...}     (player events)
//	gateway -> phone  binary [0x01][flags][0][0][seq u32 LE][Opus packet]
//	                         20 ms, 48 kHz stereo; flags bit 0 = discontinuity:
//	                         drop what is still queued before playing this one
//	                  binary [0x02][0][0][0][request id u32 LE][image bytes]
//
// Commands: see handleCommand.
package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"path/filepath"
	"time"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8099", "listen address (the tunnel connects here)")
	dataDir := flag.String("data", "data", "directory for token and webapi.json")
	librespotURL := flag.String("librespot", "http://127.0.0.1:3678", "go-librespot API")
	fifo := flag.String("fifo", "", "go-librespot's audio_output_pipe (s16le, 44.1 kHz stereo)")
	bitrate := flag.Int("bitrate", 160000, "Opus bitrate, bit/s")
	flag.Parse()
	if *fifo == "" {
		log.Fatal("-fifo is required")
	}

	token, created, err := loadToken(filepath.Join(*dataDir, "token"))
	if err != nil {
		log.Fatalf("token: %v", err)
	}
	if created {
		log.Printf("new access token saved in %s", filepath.Join(*dataDir, "token"))
	}
	web, err := loadWebAPI(filepath.Join(*dataDir, "webapi.json"))
	if err != nil {
		log.Fatalf("webapi: %v", err)
	}
	if web == nil {
		log.Printf("search off: no %s", filepath.Join(*dataDir, "webapi.json"))
	}

	gw := &gateway{
		guard: newGuard(token),
		ls:    newLibrespot(*librespotURL),
		audio: newAudioHub(),
		web:   web,
		art:   newArtCache(),
	}
	gw.clients = map[*client]struct{}{}
	go gw.audio.run(*fifo, *bitrate)
	go gw.ls.events(context.Background(), gw.onPlayerEvent)

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("ok\n")) })
	mux.HandleFunc("GET /ws", gw.serveWS)
	srv := &http.Server{Addr: *addr, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	log.Printf("Jukeberry gateway listening on %s", *addr)
	log.Fatal(srv.ListenAndServe())
}
