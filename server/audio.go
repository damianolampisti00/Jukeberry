package main

// The audio path: go-librespot's raw PCM (s16le, 44.1 kHz stereo) from a
// FIFO -> 48 kHz -> 20 ms Opus packets -> every subscribed phone.
//
// go-librespot writes to the pipe as fast as it is read (20 s of audio came
// out in about one), and it measures the playback position by what it has
// written. So the reader sets the pace.
//
// Phones that report how far they have played (audio_ack, every second) get
// the audio up to `lead` ahead of that, so a weak signal can stall the
// connection for that long without a gap; the margin stays within the track
// being heard (holdAt: a natural track change waits until the phone is
// `near` the end), so "next" and the track's title are never ahead of what
// plays. Without fresh acks (older phones, or none listening) it is one 20 ms
// block per 20 ms of wall time; when the source stalls (pause, a track
// loading) that clock restarts, with a short burst to refill the phone.

import (
	"encoding/binary"
	"io"
	"log"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	inRate      = 44100
	inBlock     = inRate / 50 // 882 stereo frames = 20 ms
	blockPeriod = 20 * time.Millisecond
	// A gap longer than this means the source stalled rather than jittered.
	stallGap = 100 * time.Millisecond
	// After a stall, this much audio goes out at once to refill the phone.
	prebuffer = 300 * time.Millisecond
	// Linux fcntl F_SETPIPE_SZ. A small pipe keeps what is queued in it,
	// and therefore the delay of a seek or skip, near one block.
	fSetPipeSz = 1031

	frameAudio     = 0x01
	flagDiscontinu = 0x01 // drop anything still queued: a seek, a new track, a restart

	lead     = 1500             // frames (30 s) a phone that acks may have ahead of what it played
	near     = 150              // frames (3 s): past a track change, wait until the phone is this close
	ackFresh = 15 * time.Minute // phones ack only while playing: a long pause keeps the margin
	// A track change this close to the old track's end is the natural one
	// (the margin may cross it later); earlier, it was a jump made elsewhere.
	naturalEndMs = 3000
)

type ackInfo struct {
	seq uint32
	at  time.Time
}

type audioHub struct {
	mu   sync.Mutex
	subs map[*client]struct{}
	acks map[*client]ackInfo
	// Set by player events (seek, new track); marks the next frame.
	flush atomic.Bool
	sent  atomic.Uint64

	// The track being read, to tell a natural track change from a jump.
	trackMu    sync.Mutex
	seq        uint32 // the next frame's sequence number
	trackSeq   uint32 // seq when it began (or was sought)
	trackPosMs int64  // its position then
	trackDurMs int64
	holding    bool // read no further until the phones are `near` holdAt
	holdAt     uint32
}

func newAudioHub() *audioHub {
	return &audioHub{subs: map[*client]struct{}{}, acks: map[*client]ackInfo{}}
}

func (h *audioHub) subscribe(c *client, on bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if on {
		h.subs[c] = struct{}{}
	} else {
		delete(h.subs, c)
		delete(h.acks, c)
	}
}

// ack: phone c has played up to frame seq. Kept even before its "audio on"
// (that command runs on its own goroutine, the ack in the read loop, so it
// may come first); played() only counts subscribed phones.
func (h *audioHub) ack(c *client, seq uint32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.acks[c] = ackInfo{seq, time.Now()}
}

// played: the furthest frame the slowest listening phone has played, if
// any of them acked recently (then the margin paces the reading).
func (h *audioHub) played() (uint32, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	var min uint32
	ok := false
	for c, a := range h.acks {
		if _, sub := h.subs[c]; !sub || time.Since(a.at) > ackFresh {
			continue
		}
		if !ok || int32(a.seq-min) < 0 {
			min = a.seq
		}
		ok = true
	}
	return min, ok
}

// trackStarted: go-librespot moved to a new track (its "metadata" event).
// A change at the old track's end is the natural one: hold the reading
// there until the phones are near it. Otherwise a jump was made elsewhere
// (another Spotify app): what the phones have queued is stale.
func (h *audioHub) trackStarted(posMs, durMs int64) {
	h.trackMu.Lock()
	defer h.trackMu.Unlock()
	readMs := h.trackPosMs + int64(h.seq-h.trackSeq)*int64(blockPeriod/time.Millisecond)
	if h.trackDurMs > 0 && readMs >= h.trackDurMs-naturalEndMs {
		h.holding, h.holdAt = true, h.seq
	} else {
		h.flush.Store(true)
	}
	h.trackSeq, h.trackPosMs, h.trackDurMs = h.seq, posMs, durMs
}

// sought: a seek in the track being read (its "seek" event).
func (h *audioHub) sought(posMs int64) {
	h.trackMu.Lock()
	defer h.trackMu.Unlock()
	h.trackSeq, h.trackPosMs, h.holding = h.seq, posMs, false
}

// paced: wait until the next block may be read, with phones that ack. False:
// none does, so the caller paces by the clock.
func (h *audioHub) paced() bool {
	for {
		played, ok := h.played()
		if !ok {
			return false
		}
		h.trackMu.Lock()
		seq, holding := h.seq, h.holding
		if holding && int32(played+near-h.holdAt) >= 0 {
			h.holding, holding = false, false
		}
		h.trackMu.Unlock()
		if !holding && int32(seq-played) < lead {
			return true
		}
		time.Sleep(blockPeriod)
	}
}

func (h *audioHub) broadcast(frame []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for c := range h.subs {
		c.sendAudio(frame)
	}
}

// run reads the FIFO forever, also when no phone listens: a stalled reader
// would stall go-librespot, and with it a playback started from another
// Spotify app on this device.
func (h *audioHub) run(fifoPath string, bitrate int) {
	enc, err := newOpusEncoder(bitrate)
	if err != nil {
		log.Fatalf("audio: %v", err)
	}
	rs, err := newResampler(inRate, outRate)
	if err != nil {
		log.Fatalf("audio: %v", err)
	}
	for {
		// O_RDWR: opening never blocks, and the read side never sees EOF
		// when go-librespot closes its end between tracks.
		f, err := os.OpenFile(fifoPath, os.O_RDWR, 0)
		if err != nil {
			log.Printf("audio: open %s: %v", fifoPath, err)
			time.Sleep(5 * time.Second)
			continue
		}
		if _, _, e := syscall.Syscall(syscall.SYS_FCNTL, f.Fd(), fSetPipeSz, 4096); e != 0 {
			log.Printf("audio: F_SETPIPE_SZ: %v", e)
		}
		log.Printf("audio: reading %s, Opus %d kbps", fifoPath, bitrate/1000)
		err = h.pump(f, enc, rs)
		f.Close()
		log.Printf("audio: %v; reopening", err)
		time.Sleep(time.Second)
	}
}

func (h *audioHub) pump(r io.Reader, enc *opusEncoder, rs *resampler) error {
	raw := make([]byte, inBlock*4)
	in := make([]int16, inBlock*2)
	out := make([]int16, 4096*2)
	pending := make([]int16, 0, frameSize*2*4)
	packet := make([]byte, maxOpusSize)
	next := time.Now()
	for {
		readStart := time.Now()
		if _, err := io.ReadFull(r, raw); err != nil {
			return err
		}
		now := time.Now()
		discontinuity := false
		_, acked := h.played()
		// A read that blocked this long means the source stalled; restart
		// the clock in the past so the first `prebuffer` goes out at once.
		// (Being behind the clock is not a stall: that is the burst itself.)
		// Phones that ack keep their margin over a pause: no discontinuity.
		if now.Sub(readStart) > stallGap || now.Sub(next) > 2*time.Second {
			next = now.Add(-prebuffer)
			discontinuity = !acked
		}
		if h.flush.Swap(false) {
			// A seek or skip: the phone drops its queue, so refill it at once.
			discontinuity = true
			if burst := now.Add(-prebuffer); next.After(burst) {
				next = burst
			}
		}
		for i := range in {
			in[i] = int16(binary.LittleEndian.Uint16(raw[i*2:]))
		}
		n, err := rs.process(in, out)
		if err != nil {
			return err
		}
		pending = append(pending, out[:n*2]...)
		for len(pending) >= frameSize*2 {
			size, err := enc.encode(pending[:frameSize*2], packet)
			if err != nil {
				return err
			}
			pending = pending[:copy(pending, pending[frameSize*2:])]
			// [type][flags][0][0][seq u32 LE][Opus packet]
			frame := make([]byte, 8+size)
			frame[0] = frameAudio
			if discontinuity {
				frame[1] = flagDiscontinu
				discontinuity = false
			}
			h.trackMu.Lock()
			binary.LittleEndian.PutUint32(frame[4:], h.seq)
			h.seq++
			h.trackMu.Unlock()
			copy(frame[8:], packet[:size])
			h.sent.Add(1)
			h.broadcast(frame)
		}
		if h.paced() {
			next = time.Now() // if the acks stop, the clock goes on from here
			continue
		}
		next = next.Add(blockPeriod)
		if d := time.Until(next); d > 0 {
			time.Sleep(d)
		}
	}
}
