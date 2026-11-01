package hub

import (
	"log/slog"
	"slices"
	"testing"

	"playerhub/server/internal/delivery"
	"playerhub/server/internal/voice"
)

type voiceFixture struct {
	*fixture
	relay *voice.Relay
}

func newVoiceFixture(t *testing.T, radius float64) *voiceFixture {
	t.Helper()
	relay, err := voice.Listen(voice.Config{Listen: "127.0.0.1:0", Radius: radius, PacketsPerSecond: 60, PacketBurst: 25},
		slog.Default())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { relay.Close() })
	f := newFixture(t)
	f.hub.opts.VoiceRadius = radius
	return &voiceFixture{fixture: f, relay: relay}
}

func (f *voiceFixture) connectVoice(session uint64, displayName string) (uint32, *voice.Endpoint) {
	f.t.Helper()
	box := delivery.NewOutbox(1024)
	endpoint, err := f.relay.Register(box)
	if err != nil {
		f.t.Fatal(err)
	}
	slot, _ := f.hub.ConnectClient(box, Identity{Session: session, DisplayName: displayName, Voice: endpoint})
	f.boxes[slot] = box
	return slot, endpoint
}

func expectTargets(t *testing.T, speaker *voice.Endpoint, want ...*voice.Endpoint) {
	t.Helper()
	got := speaker.Targets()
	if len(got) != len(want) {
		t.Fatalf("%d listeners, want %d", len(got), len(want))
	}
	for _, w := range want {
		if !slices.Contains(got, w) {
			t.Fatalf("listeners %v lack %v", got, w)
		}
	}
}

func TestWelcomeCarriesVoiceAccess(t *testing.T) {
	f := newVoiceFixture(t, 1000)
	slot, endpoint := f.connectVoice(1, "Lydia")
	welcome := f.messages(slot)[0].GetWelcome()
	if access := welcome.GetVoice(); access.GetId() != endpoint.Access().GetId() || len(access.GetKey()) != voice.KeyBytes ||
		access.GetRadius() != 1000 {
		t.Fatalf("welcome %v", welcome)
	}
	plain := f.connectAs("Serana")
	if f.messages(plain)[0].GetWelcome().GetVoice() != nil {
		t.Fatal("a connection without an endpoint was given voice access")
	}
}

func TestVoiceReachesReceiversWithinTheRadius(t *testing.T) {
	f := newVoiceFixture(t, 1000)
	a, voiceA := f.connectVoice(1, "Lydia")
	b, voiceB := f.connectVoice(2, "Serana")
	c, voiceC := f.connectVoice(3, "Aela")
	expectTargets(t, voiceA)

	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 600)
	f.placeAt(c, 1, 1800)
	f.hub.Tick()
	// Each receives the others, but only the pair within the radius hears.
	expectTargets(t, voiceA, voiceB)
	expectTargets(t, voiceB, voiceA)
	expectTargets(t, voiceC)

	// Distance is applied by the interest pass.
	f.send(c, checkpointAt(2, 1500))
	f.hub.UpdateInterest()
	expectTargets(t, voiceB, voiceA, voiceC)
	expectTargets(t, voiceC, voiceB)

	// A vanish stops the voice with the character, at the next tick. Like the character, it is
	// one-way: the vanished player still hears the one who vanished it.
	f.send(b, vanish(1, true))
	f.hub.Tick()
	expectTargets(t, voiceA)
	expectTargets(t, voiceB, voiceA, voiceC)

	// Leaving the world silences the player and takes it from every speaker's listeners.
	f.send(c, exit())
	f.hub.Tick()
	expectTargets(t, voiceC)
	expectTargets(t, voiceB, voiceA)

	f.hub.Disconnect(b)
	expectTargets(t, voiceB)
}
