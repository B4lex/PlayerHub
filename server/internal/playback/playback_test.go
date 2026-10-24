package playback

import (
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"playerhub/server/internal/recording"
	"playerhub/server/internal/wire/pb"
)

type fakePlayer struct {
	mu   sync.Mutex
	sent []*pb.ClientMessage
	left bool
}

func (p *fakePlayer) Send(m *pb.ClientMessage) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.sent = append(p.sent, m)
}

func (p *fakePlayer) Leave() { p.left = true }

func (p *fakePlayer) take() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	var words []string
	for _, m := range p.sent {
		switch body := m.Message.(type) {
		case *pb.ClientMessage_Enter:
			words = append(words, "enter")
		case *pb.ClientMessage_Exit:
			words = append(words, "exit")
		case *pb.ClientMessage_Context:
			words = append(words, fmt.Sprintf("context/e%d", body.Context.GraphEpoch))
		case *pb.ClientMessage_Checkpoint:
			c := body.Checkpoint
			word := fmt.Sprintf("cp%d/x%d", c.Sequence, c.X)
			if c.Speed == 0 {
				word += "/held"
			}
			words = append(words, word)
		case *pb.ClientMessage_Action:
			a := body.Action
			words = append(words, fmt.Sprintf("action%d/e%d/cp%d", a.ActionSequence, a.GraphEpoch, a.CheckpointSequence))
		case *pb.ClientMessage_Appearance:
			words = append(words, fmt.Sprintf("appearance%d:%s", body.Appearance.Revision, body.Appearance.Name))
		case *pb.ClientMessage_Equipment:
			words = append(words, fmt.Sprintf("equipment%d", body.Equipment.Revision))
		}
	}
	p.sent = nil
	return strings.Join(words, " ")
}

type fakeClock struct{ now time.Time }

func (c *fakeClock) Now() time.Time { return c.now }
func (c *fakeClock) advance(ms float64) {
	c.now = c.now.Add(time.Duration(ms * float64(time.Millisecond)))
}
func message(t int64, m *pb.ClientMessage) recording.Message {
	return recording.Message{TMs: t, Message: m}
}

func timeline() *recording.Recording {
	r := &recording.Recording{Header: recording.Header{ID: "r1", PlayerName: "Lydia"}, End: &recording.End{TMs: 1000, Reason: "stopped"}}
	add := func(t int64, m *pb.ClientMessage) { r.Messages = append(r.Messages, message(t, m)) }
	add(0, &pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{Revision: 7, Name: "Lydia"}}})
	add(0, &pb.ClientMessage{Message: &pb.ClientMessage_Equipment{Equipment: &pb.Equipment{Revision: 9}}})
	add(0, &pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	add(0, &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 5}}})
	for t := int64(0); t <= 1000; t += 100 {
		add(t, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{
			Sequence: uint64(t + 1000), X: int32(t / 10), Speed: 300, Flags: 2}}})
		if t == 500 {
			add(t, &pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: &pb.Action{
				ActionSequence: 40, GraphEpoch: 5, CheckpointSequence: 1500,
				Kind: &pb.Action_Attack{Attack: &pb.Attack{Action: 1}}}}})
		}
	}
	return r
}

func newSession(t *testing.T, r *recording.Recording, o Options) (*Session, *fakePlayer, *fakeClock) {
	t.Helper()
	clock := &fakeClock{now: time.Unix(0, 0)}
	player := &fakePlayer{}
	o.Now = clock.Now
	if o.Rate == 0 {
		o.Rate = 1
	}
	s, err := NewSession("p", "9", r, player, o)
	if err != nil {
		t.Fatal(err)
	}
	return s, player, clock
}

func expect(t *testing.T, got, want string) {
	t.Helper()
	if got != want {
		t.Fatalf("got  %s\nwant %s", got, want)
	}
}

func TestPlayNumbersEverythingItself(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true})
	s.Advance()
	expect(t, p.take(), "appearance1:Lydia equipment1 enter context/e5 cp1/x0")
	clock.advance(250)
	s.Advance()
	expect(t, p.take(), "cp2/x10 cp3/x20")
	clock.advance(250)
	s.Advance()
	expect(t, p.take(), "cp4/x30 cp5/x40 cp6/x50 action1/e5/cp6")
}

func TestCheckpointsCarryTheirTimelineTime(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true, Rate: 2})
	s.Advance()
	p.take()
	clock.advance(130)
	s.Advance()
	p.mu.Lock()
	defer p.mu.Unlock()
	var times []uint32
	for _, m := range p.sent {
		if c := m.GetCheckpoint(); c != nil {
			times = append(times, c.Time)
		}
	}
	if fmt.Sprint(times) != "[50 100]" {
		t.Fatalf("got times %v", times)
	}
}

func TestSeekBackStartsANewEpochWithoutResendingPresentation(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true})
	s.Advance()
	clock.advance(600)
	s.Advance()
	p.take()
	s.Seek(200)
	s.Advance()
	expect(t, p.take(), "context/e6 cp8/x20")
	clock.advance(300)
	s.Advance()
	expect(t, p.take(), "cp9/x30 cp10/x40 cp11/x50 action2/e6/cp11")
}

func TestPauseHoldsThePoseOnce(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true})
	s.Advance()
	clock.advance(150)
	s.Advance()
	p.take()
	s.Pause()
	s.Advance()
	expect(t, p.take(), "cp3/x10/held")
	clock.advance(5000)
	if delay := s.Advance(); delay >= 0 {
		t.Fatalf("a paused replay scheduled more work in %v", delay)
	}
	expect(t, p.take(), "")
	if s.State().PositionMs != 150 || s.Status() != StatusPaused {
		t.Fatalf("got %+v", s.State())
	}
}

func TestLoopRestartsWithANewEpoch(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true, Loop: true})
	s.Advance()
	clock.advance(1000)
	s.Advance()
	p.take()
	s.Advance()
	expect(t, p.take(), "context/e6 cp12/x0")
}

func TestEndHoldsAndPlayStartsOver(t *testing.T) {
	s, p, clock := newSession(t, timeline(), Options{Autoplay: true})
	s.Advance()
	clock.advance(1000)
	s.Advance()
	s.Advance()
	if s.Status() != StatusEnded {
		t.Fatalf("got %s", s.Status())
	}
	if got := p.take(); !strings.HasSuffix(got, "cp11/x100 cp12/x100/held") {
		t.Fatalf("got %s", got)
	}
	s.Play()
	s.Advance()
	expect(t, p.take(), "context/e6 cp13/x0")
}

func TestOutOfWorldStretchesExit(t *testing.T) {
	r := timeline()
	r.Messages = append(r.Messages[:4:4], []recording.Message{
		message(0, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: 1, Speed: 1}}}),
		message(100, &pb.ClientMessage{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}}),
		message(300, &pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}}),
		message(300, &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{InteriorCellId: 7, GraphEpoch: 6}}}),
		message(400, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 2, X: 2, Speed: 1}}}),
	}...)
	s, p, clock := newSession(t, r, Options{Autoplay: false, StartMs: 200})
	s.Advance()
	expect(t, p.take(), "appearance1:Lydia equipment1")
	s.Play()
	clock.advance(250)
	s.Advance()
	expect(t, p.take(), "enter context/e6 cp1/x2")
	s.Seek(50)
	s.Advance()
	expect(t, p.take(), "context/e7 cp2/x1")
	clock.advance(60)
	s.Advance()
	expect(t, p.take(), "exit")
}

func TestBotNamesFitTheLimit(t *testing.T) {
	if got := BotName("Lydia", 2); got != "Lydia #3" {
		t.Fatal(got)
	}
	long := strings.Repeat("é", 70)
	if got := BotName(long, 99); len(got) > 128 || !strings.HasSuffix(got, "é #100") {
		t.Fatalf("%q (%d bytes)", got, len(got))
	}
}
