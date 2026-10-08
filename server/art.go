package main

// Cover art for the phone, fetched here: only from Spotify's image CDNs, and
// kept in a small in-memory cache (list views ask for the same covers often).

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

const (
	artMaxBytes   = 2 << 20
	artCacheItems = 200
)

type artCache struct {
	http  *http.Client
	mu    sync.Mutex
	items map[string][]byte
	order []string
}

func newArtCache() *artCache {
	return &artCache{http: &http.Client{Timeout: 15 * time.Second}, items: map[string][]byte{}}
}

func artHostAllowed(raw string) bool {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.User != nil || u.Port() != "" {
		return false
	}
	h := strings.ToLower(u.Hostname())
	return h == "scdn.co" || strings.HasSuffix(h, ".scdn.co") ||
		h == "spotifycdn.com" || strings.HasSuffix(h, ".spotifycdn.com")
}

func (a *artCache) get(ctx context.Context, raw string) ([]byte, error) {
	if !artHostAllowed(raw) {
		return nil, errors.New("not a Spotify image URL")
	}
	a.mu.Lock()
	if b, ok := a.items[raw]; ok {
		a.mu.Unlock()
		return b, nil
	}
	a.mu.Unlock()

	req, err := http.NewRequestWithContext(ctx, "GET", raw, nil)
	if err != nil {
		return nil, err
	}
	// No redirects to other hosts.
	client := *a.http
	client.CheckRedirect = func(r *http.Request, _ []*http.Request) error {
		if !artHostAllowed(r.URL.String()) {
			return errors.New("redirect off the image CDN")
		}
		return nil
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("image: HTTP %d", resp.StatusCode)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, artMaxBytes+1))
	if err != nil {
		return nil, err
	}
	if len(b) > artMaxBytes {
		return nil, errors.New("image too large")
	}

	a.mu.Lock()
	defer a.mu.Unlock()
	if _, ok := a.items[raw]; !ok {
		a.items[raw] = b
		a.order = append(a.order, raw)
		if len(a.order) > artCacheItems {
			delete(a.items, a.order[0])
			a.order = a.order[1:]
		}
	}
	return b, nil
}
