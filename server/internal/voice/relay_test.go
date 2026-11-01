package voice

import (
	"bytes"
	"context"
	"crypto/cipher"
	"encoding/binary"
	"log/slog"
	"net"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/delivery"
	"playerhub/server/internal/wire/pb"
)

func TestReplayWindow(t *testing.T) {
	var w replayWindow
	steps := []struct {
		counter       uint32
		fresh, newest bool
	}{
		{10, true, true},
		{10, false, false},
		{12, true, true},
		{11, true, false},
		{11, false, false},
		{100, true, true},
		{36, false, false}, // 64 behind the newest
		{37, true, false},
		{37, false, false},
		{1000, true, true},
		{999, true, false},
	}
	for _, s := range steps {
		fresh, newest := w.accept(s.counter)
		if fresh != s.fresh || newest != s.newest {
			t.Fatalf("counter %d: got fresh=%v newest=%v, want %v %v", s.counter, fresh, newest, s.fresh, s.newest)
		}
	}
}

func TestVoicePlaintextIsValidated(t *testing.T) {
	good := AppendVoice(nil, Frame{Sequence: 7, End: true, Opus: []byte{1, 2, 3}})
	if f, err := parseVoice(good); err != nil || f.Sequence != 7 || !f.End || !bytes.Equal(f.Opus, []byte{1, 2, 3}) {
		t.Fatalf("got %+v %v", f, err)
	}
	for name, plaintext := range map[string][]byte{
		"short":         {1, 0},
		"no opus":       {1, 0, 0},
		"unknown flags": {1, 0, 2, 9},
		"oversized":     append([]byte{1, 0, 0}, make([]byte, 401)...),
	} {
		if _, err := parseVoice(plaintext); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

type client struct {
	t        *testing.T
	endpoint *Endpoint
	conn     *net.UDPConn
	aead     cipher.AEAD
	counter  uint32
}

type harness struct {
	t     *testing.T
	relay *Relay
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	relay, err := Listen(Config{Listen: "127.0.0.1:0", Radius: 3072, PacketsPerSecond: 60, PacketBurst: 25},
		slog.Default())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		relay.Run(ctx)
		close(done)
	}()
	t.Cleanup(func() {
		cancel()
		<-done
	})
	return &harness{t: t, relay: relay}
}

func (h *harness) connect(slot uint32) *client {
	h.t.Helper()
	endpoint, err := h.relay.Register(delivery.NewOutbox(64))
	if err != nil {
		h.t.Fatal(err)
	}
	endpoint.SetSlot(slot)
	access := endpoint.Access()
	if access.GetId() == 0 || len(access.GetKey()) != KeyBytes || access.GetPort() != uint32(h.relay.LocalAddr().Port()) ||
		access.GetRadius() != 3072 {
		h.t.Fatalf("access %v", access)
	}
	aead, err := newAEAD(access.GetKey())
	if err != nil {
		h.t.Fatal(err)
	}
	conn, err := net.DialUDP("udp", nil, net.UDPAddrFromAddrPort(h.relay.LocalAddr()))
	if err != nil {
		h.t.Fatal(err)
	}
	h.t.Cleanup(func() { conn.Close() })
	return &client{t: h.t, endpoint: endpoint, conn: conn, aead: aead}
}

func (c *client) send(kind byte, plaintext []byte) {
	c.t.Helper()
	c.counter++
	c.sendCounter(kind, c.counter, plaintext)
}

func (c *client) sendCounter(kind byte, counter uint32, plaintext []byte) {
	c.t.Helper()
	datagram := SealClient(c.aead, nil, ClientHeader{Kind: kind, ID: c.endpoint.id, Counter: counter}, plaintext)
	if _, err := c.conn.Write(datagram); err != nil {
		c.t.Fatal(err)
	}
}

func (c *client) probe(id uint32) {
	c.t.Helper()
	c.send(KindProbe, binary.LittleEndian.AppendUint32(nil, id))
	kind, plaintext := c.receive()
	if kind != KindProbe || binary.LittleEndian.Uint32(plaintext) != id {
		c.t.Fatalf("probe reply kind %d %v", kind, plaintext)
	}
}

func (c *client) receive() (byte, []byte) {
	c.t.Helper()
	buffer := make([]byte, MaximumDatagramBytes)
	c.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := c.conn.Read(buffer)
	if err != nil {
		c.t.Fatal(err)
	}
	kind, plaintext, err := OpenServer(c.aead, buffer[:n])
	if err != nil {
		c.t.Fatal(err)
	}
	return kind, plaintext
}

func (c *client) receivesNothing() {
	c.t.Helper()
	buffer := make([]byte, MaximumDatagramBytes)
	c.conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
	if n, err := c.conn.Read(buffer); err == nil {
		c.t.Fatalf("received %d bytes", n)
	}
}

func (c *client) outboxVoice() []*pb.PeerVoice {
	c.t.Helper()
	var b delivery.Batch
	if ok, _ := c.endpoint.outbox.Drain(&b, time.Now()); !ok {
		c.t.Fatal("outbox closed")
	}
	var frames []*pb.PeerVoice
	b.Write(func(frame []byte) error {
		m := &pb.ServerMessage{}
		if err := proto.Unmarshal(frame, m); err != nil {
			c.t.Fatal(err)
		}
		frames = append(frames, m.GetPeerVoice())
		return nil
	})
	return frames
}

func voiceFrame(sequence uint16, end bool, opus ...byte) []byte {
	return AppendVoice(nil, Frame{Sequence: sequence, End: end, Opus: opus})
}

func TestVoiceReachesListenersByDatagram(t *testing.T) {
	h := newHarness(t)
	speaker, listener, bystander := h.connect(1), h.connect(2), h.connect(3)
	speaker.endpoint.SetTargets([]*Endpoint{listener.endpoint})
	listener.probe(42)
	listener.endpoint.SetDatagrams(true)

	speaker.send(KindVoice, voiceFrame(5, false, 0xAA, 0xBB))
	kind, plaintext := listener.receive()
	slot, frame, err := ParsePeerVoice(plaintext)
	if kind != KindVoice || err != nil || slot != 1 || frame.Sequence != 5 || frame.End ||
		!bytes.Equal(frame.Opus, []byte{0xAA, 0xBB}) {
		t.Fatalf("kind %d slot %d frame %+v err %v", kind, slot, frame, err)
	}
	speaker.send(KindVoice, voiceFrame(6, true, 0xCC))
	_, plaintext = listener.receive()
	if _, frame, _ := ParsePeerVoice(plaintext); frame.Sequence != 6 || !frame.End {
		t.Fatalf("frame %+v", frame)
	}
	bystander.receivesNothing()
}

func TestListenerWithoutDatagramsGetsPeerVoice(t *testing.T) {
	h := newHarness(t)
	speaker, listener := h.connect(1), h.connect(2)
	speaker.endpoint.SetTargets([]*Endpoint{listener.endpoint})
	// A probe records the address, but the client has not said its datagrams arrive.
	listener.probe(1)

	speaker.send(KindVoice, voiceFrame(9, true, 1, 2, 3))
	deadline := time.Now().Add(2 * time.Second)
	var frames []*pb.PeerVoice
	for len(frames) == 0 && time.Now().Before(deadline) {
		<-listener.endpoint.outbox.Ready()
		frames = listener.outboxVoice()
	}
	if len(frames) != 1 || frames[0].GetSlot() != 1 || frames[0].GetSequence() != 9 || !frames[0].GetEnd() ||
		!bytes.Equal(frames[0].GetOpus(), []byte{1, 2, 3}) {
		t.Fatalf("frames %v", frames)
	}
	listener.receivesNothing()

	// A frame over the WebSocket takes the same way.
	if !speaker.endpoint.FrameFromClient(10, []byte{4}, false, time.Now()) {
		t.Fatal("frame from the WebSocket dropped")
	}
	if frames := listener.outboxVoice(); len(frames) != 1 || frames[0].GetSequence() != 10 {
		t.Fatalf("frames %v", frames)
	}
}

func TestForgedAndReplayedDatagramsAreDropped(t *testing.T) {
	h := newHarness(t)
	speaker, listener := h.connect(1), h.connect(2)
	speaker.endpoint.SetTargets([]*Endpoint{listener.endpoint})
	listener.probe(1)
	listener.endpoint.SetDatagrams(true)

	speaker.send(KindVoice, voiceFrame(1, false, 1))
	listener.receive()
	// The same counter again.
	speaker.sendCounter(KindVoice, speaker.counter, voiceFrame(1, false, 1))
	listener.receivesNothing()

	// Another connection's key under the speaker's ID.
	forger := *speaker
	forger.aead = listener.aead
	forger.send(KindVoice, voiceFrame(2, false, 1))
	listener.receivesNothing()

	// A tampered byte.
	speaker.counter++
	datagram := SealClient(speaker.aead, nil, ClientHeader{Kind: KindVoice, ID: speaker.endpoint.id,
		Counter: speaker.counter}, voiceFrame(3, false, 1))
	datagram[len(datagram)-1] ^= 1
	speaker.conn.Write(datagram)
	listener.receivesNothing()

	// An unknown ID.
	unknown := *speaker
	unknown.endpoint = &Endpoint{id: speaker.endpoint.id + 1}
	unknown.send(KindVoice, voiceFrame(4, false, 1))
	listener.receivesNothing()

	if _, _, rejected := h.relay.Counters(); rejected != 4 {
		t.Fatalf("rejected %d, want 4", rejected)
	}
}

func TestRemovedEndpointsNeitherSendNorReceive(t *testing.T) {
	h := newHarness(t)
	speaker, listener := h.connect(1), h.connect(2)
	speaker.endpoint.SetTargets([]*Endpoint{listener.endpoint})
	listener.probe(1)
	listener.endpoint.SetDatagrams(true)

	h.relay.Remove(listener.endpoint)
	speaker.send(KindVoice, voiceFrame(1, false, 1))
	listener.receivesNothing()
	h.relay.Remove(speaker.endpoint)
	if speaker.endpoint.FrameFromClient(2, []byte{1}, false, time.Now()) {
		t.Fatal("a removed speaker was relayed")
	}
}

func TestVoiceIsRateLimited(t *testing.T) {
	h := newHarness(t)
	speaker := h.connect(1)
	now := time.Now()
	relayed := 0
	for i := range 100 {
		if speaker.endpoint.FrameFromClient(uint32(i), []byte{1}, false, now) {
			relayed++
		}
	}
	if relayed != 25 {
		t.Fatalf("relayed %d at once, want the burst of 25", relayed)
	}
	if !speaker.endpoint.FrameFromClient(100, []byte{1}, false, now.Add(time.Second/50)) {
		t.Fatal("a frame at the refill rate was dropped")
	}
}

func TestSlowListenerKeepsTheNewestVoice(t *testing.T) {
	outbox := delivery.NewOutbox(64)
	for i := range 60 {
		outbox.AppendVoice([]byte{byte(i)})
	}
	var b delivery.Batch
	outbox.Drain(&b, time.Now())
	var written [][]byte
	b.Write(func(frame []byte) error {
		written = append(written, bytes.Clone(frame))
		return nil
	})
	if len(written) != 48 || written[0][0] != 12 || written[47][0] != 59 || outbox.VoiceDropped() != 12 {
		t.Fatalf("wrote %d from %v, dropped %d", len(written), written[0], outbox.VoiceDropped())
	}
}
