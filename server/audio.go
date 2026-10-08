package main

// The audio path: go-librespot's raw PCM (s16le, 44.1 kHz stereo) from a
// FIFO -> 48 kHz -> 20 ms Opus packets -> every subscribed phone.
//
// go-librespot writes to the pipe as fast as it is read (20 s of audio came
// out in about one), and it measures the playback position by what it has
// written. So the reader sets the pace: one 20 ms block per 20 ms of wall
// time. When the source stalls (pause, a track loading) the clock restarts,
// with a short burst to refill the phone's buffer.

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
)

type audioHub struct {
	mu   sync.Mutex
	subs map[*client]struct{}
	// Set by player events (seek, new track); marks the next frame.
	flush atomic.Bool
	sent  atomic.Uint64
}

func newAudioHub() *audioHub { return &audioHub{subs: map[*client]struct{}{}} }

func (h *audioHub) subscribe(c *client, on bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if on {
		h.subs[c] = struct{}{}
	} else {
		delete(h.subs, c)
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
	var seq uint32
	next := time.Now()
	for {
		readStart := time.Now()
		if _, err := io.ReadFull(r, raw); err != nil {
			return err
		}
		now := time.Now()
		discontinuity := false
		// A read that blocked this long means the source stalled; restart
		// the clock in the past so the first `prebuffer` goes out at once.
		// (Being behind the clock is not a stall: that is the burst itself.)
		if now.Sub(readStart) > stallGap || now.Sub(next) > 2*time.Second {
			next = now.Add(-prebuffer)
			discontinuity = true
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
			binary.LittleEndian.PutUint32(frame[4:], seq)
			copy(frame[8:], packet[:size])
			seq++
			h.sent.Add(1)
			h.broadcast(frame)
		}
		next = next.Add(blockPeriod)
		if d := time.Until(next); d > 0 {
			time.Sleep(d)
		}
	}
}
