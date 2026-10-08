package main

// Access token and brute-force lockout, as in Feather's server/guard.js:
// every connection needs `Authorization: Bearer <token>`; an address that
// gets it wrong maxFailures times is locked out for lockoutWindow.

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	maxFailures   = 10
	lockoutWindow = 15 * time.Minute
)

// loadToken reads the token file, creating it (mode 600) on first start.
func loadToken(path string) (token string, created bool, err error) {
	b, err := os.ReadFile(path)
	if err == nil {
		t := strings.TrimSpace(string(b))
		if t == "" {
			return "", false, errors.New(path + " is empty")
		}
		return t, false, nil
	}
	if !os.IsNotExist(err) {
		return "", false, err
	}
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", false, err
	}
	t := base64.RawURLEncoding.EncodeToString(raw)
	if err := os.WriteFile(path, []byte(t+"\n"), 0o600); err != nil {
		return "", false, err
	}
	return t, true, nil
}

type guard struct {
	want     [32]byte
	mu       sync.Mutex
	failures map[string]*failure
}

type failure struct {
	count        int
	since        time.Time
	blockedUntil time.Time
}

func newGuard(token string) *guard {
	return &guard{want: sha256.Sum256([]byte(token)), failures: map[string]*failure{}}
}

// check -> ok, or the remaining lockout (0 for a plain wrong token).
func (g *guard) check(ip, given string) (bool, time.Duration) {
	g.mu.Lock()
	defer g.mu.Unlock()
	now := time.Now()
	f := g.failures[ip]
	if f != nil && now.Before(f.blockedUntil) {
		return false, f.blockedUntil.Sub(now)
	}
	got := sha256.Sum256([]byte(given))
	if given != "" && subtle.ConstantTimeCompare(got[:], g.want[:]) == 1 {
		delete(g.failures, ip)
		return true, 0
	}
	if f == nil || now.Sub(f.since) > lockoutWindow {
		f = &failure{since: now}
		g.failures[ip] = f
	}
	f.count++
	if f.count >= maxFailures {
		f.blockedUntil = now.Add(lockoutWindow)
	}
	return false, 0
}

// clientIP: the peer, or Cloudflare's CF-Connecting-IP when the peer is the
// local tunnel connector.
func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		if cf := r.Header.Get("CF-Connecting-IP"); cf != "" {
			return cf
		}
	}
	return host
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if len(h) > 7 && strings.EqualFold(h[:7], "bearer ") {
		return strings.TrimSpace(h[7:])
	}
	return ""
}
