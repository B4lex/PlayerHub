package session

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"io"
	"log/slog"
	"net"
	"net/netip"
	"testing"
	"time"

	"playerhub/server/internal/config"
	"playerhub/server/internal/voice"
	"playerhub/server/internal/wire/pb"
)

func startVoiceServer(t *testing.T) *testServer {
	t.Helper()
	s := startServer(t, config.Default())
	relay, err := voice.Listen(voice.Config{Listen: "127.0.0.1:0", Radius: 3072, PacketsPerSecond: 60, PacketBurst: 25},
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go relay.Run(ctx)
	t.Cleanup(cancel)
	s.server.UseVoice(relay)
	return s
}

// voiceClient is a client's datagram side, from its Welcome's VoiceAccess.
type voiceClient struct {
	t       *testing.T
	access  *pb.VoiceAccess
	aead    cipher.AEAD
	conn    *net.UDPConn
	counter uint32
}

func dialVoice(t *testing.T, access *pb.VoiceAccess) *voiceClient {
	t.Helper()
	block, err := aes.NewCipher(access.GetKey())
	if err != nil {
		t.Fatal(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	address := netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), uint16(access.GetPort()))
	conn, err := net.DialUDP("udp", nil, net.UDPAddrFromAddrPort(address))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &voiceClient{t: t, access: access, aead: aead, conn: conn}
}

func (c *voiceClient) send(kind byte, plaintext []byte) {
	c.t.Helper()
	c.counter++
	datagram := voice.SealClient(c.aead, nil, voice.ClientHeader{Kind: kind, ID: c.access.GetId(), Counter: c.counter},
		plaintext)
	if _, err := c.conn.Write(datagram); err != nil {
		c.t.Fatal(err)
	}
}

func (c *voiceClient) receive() (byte, []byte) {
	c.t.Helper()
	buffer := make([]byte, voice.MaximumDatagramBytes)
	c.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := c.conn.Read(buffer)
	if err != nil {
		c.t.Fatal(err)
	}
	kind, plaintext, err := voice.OpenServer(c.aead, buffer[:n])
	if err != nil {
		c.t.Fatal(err)
	}
	return kind, plaintext
}

func (c *voiceClient) probe(id uint32) {
	c.t.Helper()
	c.send(voice.KindProbe, binary.LittleEndian.AppendUint32(nil, id))
	if kind, plaintext := c.receive(); kind != voice.KindProbe || binary.LittleEndian.Uint32(plaintext) != id {
		c.t.Fatalf("probe reply %d %v", kind, plaintext)
	}
}

func TestVoiceTravelsByDatagramToTheSpeakersListeners(t *testing.T) {
	s := startVoiceServer(t)
	a, welcomeA := resume(t, s, "Lydia", nil)
	b, welcomeB := resume(t, s, "Serana", nil)
	if welcomeA.GetVoice().GetId() == 0 || len(welcomeB.GetVoice().GetKey()) != voice.KeyBytes {
		t.Fatalf("welcome without voice: %v", welcomeB)
	}
	place(a, 0)
	place(b, 400)
	if enter := b.receive().GetPeerEnter(); enter.GetSlot() != welcomeA.GetSlot() {
		t.Fatalf("got %v", enter)
	}
	speaker, listener := dialVoice(t, welcomeA.GetVoice()), dialVoice(t, welcomeB.GetVoice())
	speaker.probe(1)
	listener.probe(2)
	// Voice is relayed once the hub's tick has assigned the listeners, a few milliseconds later.
	time.Sleep(50 * time.Millisecond)

	speaker.send(voice.KindVoice, voice.AppendVoice(nil, voice.Frame{Sequence: 500, End: true,
		Opus: []byte{0xF8, 0xFF, 0xFE}}))
	kind, plaintext := listener.receive()
	if kind != voice.KindVoice {
		t.Fatalf("kind %d", kind)
	}
	slot, frame, err := voice.ParsePeerVoice(plaintext)
	if err != nil || slot != welcomeA.GetSlot() || frame.Sequence != 500 || !frame.End || len(frame.Opus) != 3 {
		t.Fatalf("slot %d frame %+v err %v", slot, frame, err)
	}
}
