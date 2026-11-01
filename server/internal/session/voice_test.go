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

func voiceFrame(sequence uint32, end bool) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_VoiceFrame{VoiceFrame: &pb.VoiceFrame{
		Sequence: sequence, Opus: []byte{0xF8, 0xFF, 0xFE}, End: end}}}
}

// receivePeerVoice skips presentation until the first voice frame.
func (c *client) receivePeerVoice() *pb.PeerVoice {
	c.t.Helper()
	for range 32 {
		if v := c.receive().GetPeerVoice(); v != nil {
			return v
		}
	}
	c.t.Fatal("no voice")
	return nil
}

func TestVoiceTravelsTheWebSocketUntilTheClientTakesDatagrams(t *testing.T) {
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
	// Voice is relayed once the hub's tick has assigned the listeners, a few milliseconds later.
	time.Sleep(50 * time.Millisecond)

	// Voice has its own allowance, so a burst of it does not close the connection.
	for sequence := range uint32(200) {
		a.send(voiceFrame(sequence, sequence == 199))
	}
	v := b.receivePeerVoice()
	if v.GetSlot() != welcomeA.GetSlot() || v.GetSequence() != 0 || len(v.GetOpus()) != 3 {
		t.Fatalf("got %v", v)
	}
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 3}}})
	for {
		if pong := a.receive().GetPong(); pong != nil {
			break
		}
	}

	// B's datagrams reach the server, and B says so.
	access := welcomeB.GetVoice()
	block, err := aes.NewCipher(access.GetKey())
	if err != nil {
		t.Fatal(err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	port := uint16(access.GetPort())
	udp, err := net.DialUDP("udp", nil, net.UDPAddrFromAddrPort(netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), port)))
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	probe := voice.SealClient(aead, nil, voice.ClientHeader{Kind: voice.KindProbe, ID: access.GetId(), Counter: 1},
		binary.LittleEndian.AppendUint32(nil, 77))
	if _, err := udp.Write(probe); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, voice.MaximumDatagramBytes)
	udp.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err := udp.Read(buffer)
	if err != nil {
		t.Fatal(err)
	}
	if kind, plaintext, err := voice.OpenServer(aead, buffer[:n]); err != nil || kind != voice.KindProbe ||
		binary.LittleEndian.Uint32(plaintext) != 77 {
		t.Fatalf("probe reply %d %v %v", kind, plaintext, err)
	}
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_VoiceRoute{VoiceRoute: &pb.VoiceRoute{Datagrams: true}}})
	time.Sleep(50 * time.Millisecond)

	// The allowance has refilled by a frame or more.
	time.Sleep(100 * time.Millisecond)
	a.send(voiceFrame(500, true))
	udp.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, err = udp.Read(buffer)
	if err != nil {
		t.Fatal(err)
	}
	kind, plaintext, err := voice.OpenServer(aead, buffer[:n])
	if err != nil || kind != voice.KindVoice {
		t.Fatalf("kind %d err %v", kind, err)
	}
	slot, frame, err := voice.ParsePeerVoice(plaintext)
	if err != nil || slot != welcomeA.GetSlot() || frame.Sequence != 500 || !frame.End {
		t.Fatalf("slot %d frame %+v err %v", slot, frame, err)
	}
}
