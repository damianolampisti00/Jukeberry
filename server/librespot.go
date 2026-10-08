package main

// go-librespot's local API: player control and library (HTTP), player
// events (WebSocket /events).

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/coder/websocket"
)

type librespot struct {
	base string
	http *http.Client
}

func newLibrespot(base string) *librespot {
	return &librespot{base: strings.TrimRight(base, "/"), http: &http.Client{Timeout: 20 * time.Second}}
}

// call -> HTTP status and body (raw JSON, or nil when empty).
func (l *librespot) call(ctx context.Context, method, path string, body any) (int, json.RawMessage, error) {
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return 0, nil, err
		}
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, method, l.base+path, rd)
	if err != nil {
		return 0, nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := l.http.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return resp.StatusCode, nil, err
	}
	data = bytes.TrimSpace(data)
	if len(data) == 0 || !json.Valid(data) {
		if resp.StatusCode >= 400 && len(data) > 0 {
			return resp.StatusCode, nil, fmt.Errorf("%s", truncate(string(data), 200))
		}
		return resp.StatusCode, nil, nil
	}
	return resp.StatusCode, json.RawMessage(data), nil
}

type playerEvent struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data"`
}

// events relays /events to onEvent forever, reconnecting.
func (l *librespot) events(ctx context.Context, onEvent func(playerEvent)) {
	url := "ws" + strings.TrimPrefix(l.base, "http") + "/events"
	for ctx.Err() == nil {
		conn, _, err := websocket.Dial(ctx, url, nil)
		if err != nil {
			time.Sleep(3 * time.Second)
			continue
		}
		conn.SetReadLimit(1 << 20)
		log.Printf("librespot: events connected")
		for {
			_, msg, err := conn.Read(ctx)
			if err != nil {
				log.Printf("librespot: events: %v", err)
				break
			}
			var ev playerEvent
			if json.Unmarshal(msg, &ev) == nil && ev.Type != "" {
				onEvent(ev)
			}
		}
		conn.CloseNow()
		time.Sleep(2 * time.Second)
	}
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n] + "…"
	}
	return s
}
