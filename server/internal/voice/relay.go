// Package voice relays players' Opus frames to the listeners the hub assigns them, by encrypted UDP
// datagram or, for a client its datagrams do not reach, through its WebSocket outbox.
package voice

import (
	"context"
	"crypto/cipher"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"log/slog"
	"net"
	"net/netip"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"playerhub/server/internal/delivery"
	"playerhub/server/internal/limit"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

type Config struct {
	// UDP address the relay reads datagrams on.
	Listen string
	// Told to clients; an empty host means the host the client connected to, and port 0 the
	// listening port.
	PublicHost string
	PublicPort int
	// Clients fade a speaker out towards it; the hub sends no frames beyond it.
	Radius           float64
	PacketsPerSecond float64
	PacketBurst      float64
}

type Relay struct {
	config Config
	port   uint32
	conn   *net.UDPConn
	log    *slog.Logger

	mu        sync.RWMutex
	endpoints map[uint32]*Endpoint

	datagramsIn  atomic.Uint64
	datagramsOut atomic.Uint64
	rejected     atomic.Uint64
}

// Endpoint is one connection's voice: its key and datagram address, and the listeners the hub
// assigned it.
type Endpoint struct {
	relay  *Relay
	id     uint32
	key    []byte
	aead   cipher.AEAD
	outbox *delivery.Outbox
	slot   atomic.Uint32

	mu      sync.Mutex
	window  replayWindow
	address netip.AddrPort
	rate    limit.TokenBucket

	datagrams atomic.Bool
	closed    atomic.Bool
	counter   atomic.Uint32
	targets   atomic.Pointer[[]*Endpoint]
}

var scratch = sync.Pool{New: func() any {
	b := make([]byte, 0, MaximumDatagramBytes)
	return &b
}}

// Listen opens the UDP socket; Run reads it.
func Listen(c Config, log *slog.Logger) (*Relay, error) {
	address, err := net.ResolveUDPAddr("udp", c.Listen)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", address)
	if err != nil {
		return nil, err
	}
	port := uint32(c.PublicPort)
	if port == 0 {
		port = uint32(conn.LocalAddr().(*net.UDPAddr).Port)
	}
	return &Relay{config: c, port: port, conn: conn, log: log, endpoints: make(map[uint32]*Endpoint)}, nil
}

// Close stops Run's readers; Run closes the socket itself when its context ends.
func (r *Relay) Close() error {
	return r.conn.Close()
}

func (r *Relay) LocalAddr() netip.AddrPort {
	return r.conn.LocalAddr().(*net.UDPAddr).AddrPort()
}

// Run reads datagrams on one goroutine per CPU until ctx ends, then closes the socket.
func (r *Relay) Run(ctx context.Context) {
	var wg sync.WaitGroup
	for range max(1, runtime.NumCPU()) {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r.read()
		}()
	}
	<-ctx.Done()
	r.conn.Close()
	wg.Wait()
}

func (r *Relay) read() {
	buffer := make([]byte, MaximumDatagramBytes+1)
	var plaintext, reply []byte
	for {
		n, from, err := r.conn.ReadFromUDPAddrPort(buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		r.datagramsIn.Add(1)
		datagram := buffer[:n]
		h, ok := ParseClientHeader(datagram)
		if !ok || n > MaximumDatagramBytes {
			r.rejected.Add(1)
			continue
		}
		r.mu.RLock()
		e := r.endpoints[h.ID]
		r.mu.RUnlock()
		if e == nil {
			r.rejected.Add(1)
			continue
		}
		plaintext, err = openClient(e.aead, plaintext[:0], datagram, h)
		if err != nil || !e.accept(h.Counter, from) {
			r.rejected.Add(1)
			continue
		}
		switch h.Kind {
		case KindProbe:
			if len(plaintext) != ProbeBytes {
				r.rejected.Add(1)
				continue
			}
			// Answered from where it came, which is the address now on record unless an older
			// datagram arrived late from one the client left.
			reply = sealServer(e.aead, reply[:0], KindProbe, e.counter.Add(1), plaintext)
			if _, err := r.conn.WriteToUDPAddrPort(reply, from); err == nil {
				r.datagramsOut.Add(1)
			}
		case KindVoice:
			frame, err := parseVoice(plaintext)
			if err != nil {
				r.rejected.Add(1)
				continue
			}
			r.Forward(e, frame, time.Now())
		}
	}
}

// Register gives a connection its voice ID and key; Remove ends it.
func (r *Relay) Register(outbox *delivery.Outbox) (*Endpoint, error) {
	key := make([]byte, KeyBytes)
	if _, err := rand.Read(key); err != nil {
		return nil, err
	}
	aead, err := newAEAD(key)
	if err != nil {
		return nil, err
	}
	e := &Endpoint{relay: r, key: key, aead: aead, outbox: outbox}
	e.targets.Store(&[]*Endpoint{})
	r.mu.Lock()
	defer r.mu.Unlock()
	for e.id == 0 || r.endpoints[e.id] != nil {
		var id [4]byte
		if _, err := rand.Read(id[:]); err != nil {
			return nil, err
		}
		e.id = binary.LittleEndian.Uint32(id[:])
	}
	r.endpoints[e.id] = e
	return e, nil
}

func (r *Relay) Remove(e *Endpoint) {
	e.closed.Store(true)
	e.targets.Store(&[]*Endpoint{})
	r.mu.Lock()
	delete(r.endpoints, e.id)
	r.mu.Unlock()
}

// Counters gives the datagrams read, written, and rejected since the start.
func (r *Relay) Counters() (in, out, rejected uint64) {
	return r.datagramsIn.Load(), r.datagramsOut.Load(), r.rejected.Load()
}

// Forward sends one frame of e's voice to each of its listeners, by datagram to those that
// receive them and through the outbox to the others. Frames over e's rate are dropped.
func (r *Relay) Forward(e *Endpoint, f Frame, now time.Time) bool {
	if e.closed.Load() || !e.allow(now, r.config.PacketsPerSecond, r.config.PacketBurst) {
		return false
	}
	slot := e.slot.Load()
	plaintextBuffer, datagramBuffer := scratch.Get().(*[]byte), scratch.Get().(*[]byte)
	defer scratch.Put(plaintextBuffer)
	defer scratch.Put(datagramBuffer)
	var plaintext, message []byte
	datagram := (*datagramBuffer)[:0]
	for _, target := range *e.targets.Load() {
		if target.closed.Load() {
			continue
		}
		if address, ok := target.datagramAddress(); ok {
			if plaintext == nil {
				plaintext = appendPeerVoice((*plaintextBuffer)[:0], slot, f)
			}
			datagram = sealServer(target.aead, datagram[:0], KindVoice, target.counter.Add(1), plaintext)
			if _, err := r.conn.WriteToUDPAddrPort(datagram, address); err == nil {
				r.datagramsOut.Add(1)
			}
			continue
		}
		if target.outbox == nil {
			continue
		}
		if message == nil {
			message = wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerVoice{PeerVoice: &pb.PeerVoice{
				Slot: slot, Sequence: uint32(f.Sequence), Opus: f.Opus, End: f.End}}})
		}
		target.outbox.AppendVoice(message)
	}
	return true
}

// Access is what the Welcome tells the client.
func (e *Endpoint) Access() *pb.VoiceAccess {
	c := e.relay.config
	return &pb.VoiceAccess{Host: c.PublicHost, Port: e.relay.port, Id: e.id, Key: e.key, Radius: uint32(c.Radius)}
}

func (e *Endpoint) SetSlot(slot uint32) {
	e.slot.Store(slot)
}

// SetDatagrams records the client's VoiceRoute.
func (e *Endpoint) SetDatagrams(datagrams bool) {
	e.datagrams.Store(datagrams)
}

// SetTargets replaces the listeners; the caller does not change the slice afterwards.
func (e *Endpoint) SetTargets(targets []*Endpoint) {
	e.targets.Store(&targets)
}

func (e *Endpoint) Targets() []*Endpoint {
	return *e.targets.Load()
}

// FrameFromClient relays a frame that arrived on the client's WebSocket.
func (e *Endpoint) FrameFromClient(sequence uint32, opus []byte, end bool, now time.Time) bool {
	return e.relay.Forward(e, Frame{Sequence: uint16(sequence), End: end, Opus: opus}, now)
}

func (e *Endpoint) accept(counter uint32, from netip.AddrPort) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	fresh, newest := e.window.accept(counter)
	// Only the newest datagram moves the address, so a late one from an address the client left
	// does not take it back.
	if newest {
		e.address = from
	}
	return fresh
}

func (e *Endpoint) allow(now time.Time, rate, burst float64) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.rate.Allow(now, rate, burst)
}

func (e *Endpoint) datagramAddress() (netip.AddrPort, bool) {
	if !e.datagrams.Load() {
		return netip.AddrPort{}, false
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.address, e.address.IsValid()
}
