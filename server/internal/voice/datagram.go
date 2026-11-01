package voice

import (
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"errors"

	"playerhub/server/internal/validate"
)

// Datagram kinds, the first byte of every datagram in either direction.
const (
	KindVoice byte = 1
	KindProbe byte = 2
)

const (
	KeyBytes = 32
	TagBytes = 16

	// kind, voice ID, counter
	ClientHeaderBytes = 1 + 4 + 4
	// kind, counter
	ServerHeaderBytes = 1 + 4
	// sequence, flags
	VoiceHeaderBytes = 2 + 1
	// speaker slot, sequence, flags
	PeerVoiceHeaderBytes = 4 + 2 + 1
	ProbeBytes           = 4

	FlagEnd byte = 1

	MaximumDatagramBytes = ClientHeaderBytes + PeerVoiceHeaderBytes + validate.MaximumVoiceFrameBytes + TagBytes

	directionToServer byte = 1
	directionToClient byte = 2
	nonceBytes             = 12
)

var errDatagram = errors.New("malformed voice datagram")

func newAEAD(key []byte) (cipher.AEAD, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	return cipher.NewGCM(block)
}

// The counter is unique per key and direction, so the nonce never repeats.
func nonce(direction byte, counter uint32) [nonceBytes]byte {
	var n [nonceBytes]byte
	n[0] = direction
	binary.LittleEndian.PutUint32(n[4:], counter)
	return n
}

// ClientHeader is the clear part of a datagram from a client, authenticated as associated data.
type ClientHeader struct {
	Kind    byte
	ID      uint32
	Counter uint32
}

func ParseClientHeader(b []byte) (ClientHeader, bool) {
	if len(b) < ClientHeaderBytes+TagBytes || (b[0] != KindVoice && b[0] != KindProbe) {
		return ClientHeader{}, false
	}
	return ClientHeader{Kind: b[0], ID: binary.LittleEndian.Uint32(b[1:]), Counter: binary.LittleEndian.Uint32(b[5:])},
		true
}

// SealClient appends a client's datagram to dst; clients and tests use the same layout.
func SealClient(aead cipher.AEAD, dst []byte, h ClientHeader, plaintext []byte) []byte {
	start := len(dst)
	dst = append(dst, h.Kind)
	dst = binary.LittleEndian.AppendUint32(dst, h.ID)
	dst = binary.LittleEndian.AppendUint32(dst, h.Counter)
	n := nonce(directionToServer, h.Counter)
	return aead.Seal(dst, n[:], plaintext, dst[start:])
}

func openClient(aead cipher.AEAD, dst, datagram []byte, h ClientHeader) ([]byte, error) {
	n := nonce(directionToServer, h.Counter)
	return aead.Open(dst, n[:], datagram[ClientHeaderBytes:], datagram[:ClientHeaderBytes])
}

func sealServer(aead cipher.AEAD, dst []byte, kind byte, counter uint32, plaintext []byte) []byte {
	start := len(dst)
	dst = append(dst, kind)
	dst = binary.LittleEndian.AppendUint32(dst, counter)
	n := nonce(directionToClient, counter)
	return aead.Seal(dst, n[:], plaintext, dst[start:])
}

// OpenServer opens a datagram the server sent; for tests and tools acting as a client.
func OpenServer(aead cipher.AEAD, datagram []byte) (kind byte, plaintext []byte, err error) {
	if len(datagram) < ServerHeaderBytes+TagBytes {
		return 0, nil, errDatagram
	}
	n := nonce(directionToClient, binary.LittleEndian.Uint32(datagram[1:]))
	plaintext, err = aead.Open(nil, n[:], datagram[ServerHeaderBytes:], datagram[:ServerHeaderBytes])
	return datagram[0], plaintext, err
}

// Frame is one Opus frame of a speaker's voice.
type Frame struct {
	Sequence uint16
	End      bool
	Opus     []byte
}

func parseVoice(plaintext []byte) (Frame, error) {
	if len(plaintext) < VoiceHeaderBytes {
		return Frame{}, errDatagram
	}
	f := Frame{Sequence: binary.LittleEndian.Uint16(plaintext), End: plaintext[2]&FlagEnd != 0,
		Opus: plaintext[VoiceHeaderBytes:]}
	if plaintext[2]&^FlagEnd != 0 || validate.VoiceFrame(uint32(f.Sequence), f.Opus) != nil {
		return Frame{}, errDatagram
	}
	return f, nil
}

func AppendVoice(dst []byte, f Frame) []byte {
	dst = binary.LittleEndian.AppendUint16(dst, f.Sequence)
	dst = append(dst, flags(f))
	return append(dst, f.Opus...)
}

func appendPeerVoice(dst []byte, slot uint32, f Frame) []byte {
	dst = binary.LittleEndian.AppendUint32(dst, slot)
	return AppendVoice(dst, f)
}

// ParsePeerVoice reads a relayed frame; for tests and tools acting as a client.
func ParsePeerVoice(plaintext []byte) (slot uint32, f Frame, err error) {
	if len(plaintext) < PeerVoiceHeaderBytes {
		return 0, Frame{}, errDatagram
	}
	f, err = parseVoice(plaintext[4:])
	return binary.LittleEndian.Uint32(plaintext), f, err
}

func flags(f Frame) byte {
	if f.End {
		return FlagEnd
	}
	return 0
}

// replayWindow accepts each counter once, and older ones only within the last 64.
type replayWindow struct {
	seen    bool
	highest uint32
	bits    uint64
}

// accept reports whether counter is new, and whether it is the newest so far.
func (w *replayWindow) accept(counter uint32) (fresh, newest bool) {
	if !w.seen {
		w.seen, w.highest, w.bits = true, counter, 1
		return true, true
	}
	if counter > w.highest {
		if shift := counter - w.highest; shift >= 64 {
			w.bits = 1
		} else {
			w.bits = w.bits<<shift | 1
		}
		w.highest = counter
		return true, true
	}
	age := w.highest - counter
	if age >= 64 || w.bits&(1<<age) != 0 {
		return false, false
	}
	w.bits |= 1 << age
	return true, false
}
