package main

// Resampling (libsoxr) and Opus encoding (libopus) through cgo. Spotify's
// audio comes out of go-librespot at 44.1 kHz; Opus only runs at 48 kHz.

/*
#cgo pkg-config: opus soxr
#include <stdlib.h>
#include <opus.h>
#include <soxr.h>

// opus_encoder_ctl is variadic, which cgo can't call: wrap it.
static OpusEncoder *jb_opus_new(int bitrate, int *err) {
	OpusEncoder *e = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, err);
	if (!e) return NULL;
	opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(10));
	opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
	opus_encoder_ctl(e, OPUS_SET_VBR(1));
	return e;
}

static soxr_t jb_soxr_new(double in_rate, double out_rate, soxr_error_t *err) {
	soxr_io_spec_t io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
	soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, 0);
	return soxr_create(in_rate, out_rate, 2, err, &io, &q, NULL);
}
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

const (
	outRate     = 48000
	frameSize   = 960 // samples per channel in one 20 ms Opus frame at 48 kHz
	maxOpusSize = 4000
)

type opusEncoder struct{ e *C.OpusEncoder }

func newOpusEncoder(bitrate int) (*opusEncoder, error) {
	var cerr C.int
	e := C.jb_opus_new(C.int(bitrate), &cerr)
	if e == nil {
		return nil, fmt.Errorf("opus_encoder_create: %s", C.GoString(C.opus_strerror(cerr)))
	}
	return &opusEncoder{e: e}, nil
}

// encode turns one frame (frameSize interleaved stereo samples) into a packet.
func (o *opusEncoder) encode(pcm []int16, out []byte) (int, error) {
	if len(pcm) != frameSize*2 {
		return 0, errors.New("opus: wrong frame length")
	}
	n := C.opus_encode(o.e, (*C.opus_int16)(unsafe.Pointer(&pcm[0])), frameSize,
		(*C.uchar)(unsafe.Pointer(&out[0])), C.opus_int32(len(out)))
	if n < 0 {
		return 0, fmt.Errorf("opus_encode: %s", C.GoString(C.opus_strerror(C.int(n))))
	}
	return int(n), nil
}

type resampler struct{ s C.soxr_t }

func newResampler(inRate, outRate float64) (*resampler, error) {
	var cerr C.soxr_error_t
	s := C.jb_soxr_new(C.double(inRate), C.double(outRate), &cerr)
	if cerr != nil {
		return nil, fmt.Errorf("soxr_create: %s", C.GoString(cerr))
	}
	return &resampler{s: s}, nil
}

// process resamples interleaved stereo `in` into `out`; returns the number
// of stereo frames written. soxr keeps a little history, so the output of
// one call is not exactly len(in) * ratio.
func (r *resampler) process(in, out []int16) (int, error) {
	var idone, odone C.size_t
	cerr := C.soxr_process(r.s,
		C.soxr_in_t(unsafe.Pointer(&in[0])), C.size_t(len(in)/2), &idone,
		C.soxr_out_t(unsafe.Pointer(&out[0])), C.size_t(len(out)/2), &odone)
	if cerr != nil {
		return 0, fmt.Errorf("soxr_process: %s", C.GoString(cerr))
	}
	return int(odone), nil
}
