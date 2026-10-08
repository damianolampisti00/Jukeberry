package main

// Search through Spotify's public Web API, with the user's own developer app
// (client credentials: no user login involved). go-librespot's session token
// can't be used for it: that token belongs to a client ID shared by every
// librespot install, and its Web API quota is always exhausted (429).
//
// webapi.json: {"client_id": "...", "client_secret": "...", "market": "IT"}

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

type webAPI struct {
	ClientID     string `json:"client_id"`
	ClientSecret string `json:"client_secret"`
	Market       string `json:"market"`

	http   *http.Client
	mu     sync.Mutex
	token  string
	expiry time.Time
}

var errSearchOff = errors.New("search is not configured (webapi.json)")

func loadWebAPI(path string) (*webAPI, error) {
	b, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	w := &webAPI{http: &http.Client{Timeout: 15 * time.Second}}
	if err := json.Unmarshal(b, w); err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	if w.ClientID == "" || w.ClientSecret == "" {
		return nil, fmt.Errorf("%s: client_id and client_secret are required", path)
	}
	return w, nil
}

func (w *webAPI) accessToken(ctx context.Context) (string, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.token != "" && time.Now().Before(w.expiry) {
		return w.token, nil
	}
	form := url.Values{"grant_type": {"client_credentials"}}
	req, _ := http.NewRequestWithContext(ctx, "POST", "https://accounts.spotify.com/api/token", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.SetBasicAuth(w.ClientID, w.ClientSecret)
	resp, err := w.http.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	var t struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
		Error       string `json:"error"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<16)).Decode(&t); err != nil {
		return "", fmt.Errorf("token: HTTP %d", resp.StatusCode)
	}
	if t.AccessToken == "" {
		return "", fmt.Errorf("token: HTTP %d %s", resp.StatusCode, t.Error)
	}
	w.token = t.AccessToken
	w.expiry = time.Now().Add(time.Duration(t.ExpiresIn-60) * time.Second)
	return w.token, nil
}

// Trimmed results: only what the phone shows.
type searchItem struct {
	Kind     string `json:"kind"` // track | album | artist | playlist
	URI      string `json:"uri"`
	Name     string `json:"name"`
	Subtitle string `json:"subtitle,omitempty"` // artists, or the playlist owner
	Image    string `json:"image,omitempty"`
	Duration int    `json:"duration,omitempty"` // ms, tracks only
}

type apiImage struct {
	URL   string `json:"url"`
	Width int    `json:"width"`
}

type apiNamed struct {
	Name string `json:"name"`
}

// smallestImage: the smallest picture at least 64 px wide (list thumbnails).
func smallestImage(imgs []apiImage) string {
	best := ""
	bestW := 1 << 30
	for _, im := range imgs {
		if best == "" || im.Width >= 64 && (bestW < 64 || im.Width < bestW) {
			best, bestW = im.URL, im.Width
		}
	}
	return best
}

func joinNames(ns []apiNamed) string {
	parts := make([]string, 0, len(ns))
	for _, n := range ns {
		parts = append(parts, n.Name)
	}
	return strings.Join(parts, ", ")
}

func (w *webAPI) search(ctx context.Context, q, types string, limit, offset int) (map[string][]searchItem, error) {
	if w == nil {
		return nil, errSearchOff
	}
	tok, err := w.accessToken(ctx)
	if err != nil {
		return nil, err
	}
	v := url.Values{"q": {q}, "type": {types}, "limit": {fmt.Sprint(limit)}, "offset": {fmt.Sprint(offset)}}
	if w.Market != "" {
		v.Set("market", w.Market)
	}
	req, _ := http.NewRequestWithContext(ctx, "GET", "https://api.spotify.com/v1/search?"+v.Encode(), nil)
	req.Header.Set("Authorization", "Bearer "+tok)
	resp, err := w.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		return nil, fmt.Errorf("search: HTTP %d %s", resp.StatusCode, truncate(string(b), 200))
	}
	var r struct {
		Tracks *struct {
			Items []*struct {
				URI        string     `json:"uri"`
				Name       string     `json:"name"`
				DurationMs int        `json:"duration_ms"`
				Artists    []apiNamed `json:"artists"`
				Album      struct {
					Images []apiImage `json:"images"`
				} `json:"album"`
			} `json:"items"`
		} `json:"tracks"`
		Albums *struct {
			Items []*struct {
				URI     string     `json:"uri"`
				Name    string     `json:"name"`
				Artists []apiNamed `json:"artists"`
				Images  []apiImage `json:"images"`
			} `json:"items"`
		} `json:"albums"`
		Artists *struct {
			Items []*struct {
				URI    string     `json:"uri"`
				Name   string     `json:"name"`
				Images []apiImage `json:"images"`
			} `json:"items"`
		} `json:"artists"`
		Playlists *struct {
			Items []*struct {
				URI    string `json:"uri"`
				Name   string `json:"name"`
				Owner  struct {
					DisplayName string `json:"display_name"`
				} `json:"owner"`
				Images []apiImage `json:"images"`
			} `json:"items"`
		} `json:"playlists"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 4<<20)).Decode(&r); err != nil {
		return nil, err
	}
	out := map[string][]searchItem{}
	// Items can be null in Spotify's answers (removed content): skip them.
	if r.Tracks != nil {
		for _, t := range r.Tracks.Items {
			if t != nil {
				out["tracks"] = append(out["tracks"], searchItem{"track", t.URI, t.Name, joinNames(t.Artists), smallestImage(t.Album.Images), t.DurationMs})
			}
		}
	}
	if r.Albums != nil {
		for _, a := range r.Albums.Items {
			if a != nil {
				out["albums"] = append(out["albums"], searchItem{"album", a.URI, a.Name, joinNames(a.Artists), smallestImage(a.Images), 0})
			}
		}
	}
	if r.Artists != nil {
		for _, a := range r.Artists.Items {
			if a != nil {
				out["artists"] = append(out["artists"], searchItem{"artist", a.URI, a.Name, "", smallestImage(a.Images), 0})
			}
		}
	}
	if r.Playlists != nil {
		for _, p := range r.Playlists.Items {
			if p != nil {
				out["playlists"] = append(out["playlists"], searchItem{"playlist", p.URI, p.Name, p.Owner.DisplayName, smallestImage(p.Images), 0})
			}
		}
	}
	return out, nil
}
