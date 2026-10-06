package delivery

import (
	"slices"
	"strings"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

func entry(peer uint32, sequence uint64) []byte {
	return wire.AppendEntry(nil, peer, &pb.Checkpoint{Sequence: sequence})
}

func leave(peer uint32) []byte {
	return wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerLeave{PeerLeave: &pb.PeerLeave{Slot: peer}}})
}

func action(peer uint32, sequence uint64) []byte {
	return wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerAction{PeerAction: &pb.PeerAction{
		Slot: peer, Action: &pb.Action{ActionSequence: sequence}}}})
}

func appearance(peer uint32, revision uint64) []byte {
	return wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerAppearance{PeerAppearance: &pb.PeerAppearance{
		Slot: peer, Appearance: &pb.Appearance{Revision: revision}}}})
}

func written(t *testing.T, o *Outbox) []string {
	t.Helper()
	var b Batch
	if ok, _ := o.Drain(&b, time.Now()); !ok {
		t.Fatal("drain refused")
	}
	var lines []string
	err := b.Write(func(frame []byte) error {
		var m pb.ServerMessage
		if err := proto.Unmarshal(frame, &m); err != nil {
			return err
		}
		switch body := m.Message.(type) {
		case *pb.ServerMessage_PeerStates:
			var states []string
			for _, s := range body.PeerStates.States {
				states = append(states, format(s.Player, s.Sequence))
			}
			slices.Sort(states)
			lines = append(lines, "states "+strings.Join(states, " "))
		case *pb.ServerMessage_PeerLeave:
			lines = append(lines, "leave "+format(body.PeerLeave.Slot, 0))
		case *pb.ServerMessage_PeerAction:
			lines = append(lines, "action "+format(body.PeerAction.Slot, body.PeerAction.Action.ActionSequence))
		case *pb.ServerMessage_PeerAppearance:
			lines = append(lines, "appearance "+format(body.PeerAppearance.Slot, body.PeerAppearance.Appearance.Revision))
		default:
			t.Fatalf("unexpected %T", body)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return lines
}

func format(peer uint32, n uint64) string {
	s := string(rune('A' + peer - 1))
	if n != 0 {
		s += ":" + string(rune('0'+n))
	}
	return s
}

func expect(t *testing.T, got []string, want ...string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("got %q, want %q", got, want)
	}
	for i := range got {
		if got[i] != want[i] {
			t.Fatalf("got %q, want %q", got, want)
		}
	}
}

func set(o *Outbox, peer uint32, sequence uint64) {
	o.SetCheckpoint(peer, sequence, entry(peer, sequence), time.Now())
}

func TestLatestWinsCheckpointsShareOneBatch(t *testing.T) {
	o := NewOutbox(16)
	set(o, 1, 1)
	set(o, 1, 2)
	set(o, 2, 5)
	expect(t, written(t, o), "states A:2 B:5")
}

func TestOrderedMessageIsNotOvertakenByItsPeersCheckpoint(t *testing.T) {
	o := NewOutbox(16)
	set(o, 1, 3)
	set(o, 2, 4)
	o.AppendAnchored(1, 3, 3, entry(1, 3), action(1, 1))
	set(o, 1, 5)
	expect(t, written(t, o), "states A:3", "action A:1", "states A:5 B:4")
}

func TestAnchorIsForcedWhenTheRecipientHoldsAnOlderCheckpoint(t *testing.T) {
	o := NewOutbox(16)
	o.Enter(1, 2, leave(9))
	written(t, o)
	o.AppendAnchored(1, 4, 4, entry(1, 4), action(1, 1))
	o.AppendAnchored(1, 4, 4, entry(1, 4), action(1, 2))
	expect(t, written(t, o), "states A:4", "action A:1", "action A:2")
}

func TestLeaveDiscardsThePendingCheckpoint(t *testing.T) {
	o := NewOutbox(16)
	set(o, 1, 3)
	o.Leave(1, leave(1))
	expect(t, written(t, o), "leave A")
	if o.Queued(1) != 0 {
		t.Fatal("left peer still has a queued sequence")
	}
}

func TestReplaceDropsTheUnsentMessage(t *testing.T) {
	o := NewOutbox(16)
	o.Replace(1, ReplaceAppearance, appearance(1, 1))
	o.AppendAnchored(1, 1, 1, entry(1, 1), action(1, 1))
	o.Replace(1, ReplaceAppearance, appearance(1, 2))
	o.Replace(2, ReplaceAppearance, appearance(2, 1))
	expect(t, written(t, o), "states A:1", "action A:1", "appearance A:2", "appearance B:1")
}

func TestReplacementsDoNotCountTowardsTheBacklog(t *testing.T) {
	o := NewOutbox(2)
	for revision := uint64(1); revision <= 5; revision++ {
		o.Replace(1, ReplaceAppearance, appearance(1, revision))
	}
	expect(t, written(t, o), "appearance A:5")
}

func TestOverflowRefusesTheDrain(t *testing.T) {
	o := NewOutbox(2)
	for i := range uint64(3) {
		o.AppendGlobal(action(1, i+1))
	}
	var b Batch
	if ok, _ := o.Drain(&b, time.Now()); ok {
		t.Fatal("overflowed outbox drained")
	}
}

func TestSignalOnlyForOrderedTraffic(t *testing.T) {
	o := NewOutbox(4)
	set(o, 1, 1)
	select {
	case <-o.Ready():
		t.Fatal("checkpoint signalled")
	default:
	}
	o.AppendGlobal(action(1, 1))
	select {
	case <-o.Ready():
	default:
		t.Fatal("ordered message did not signal")
	}
}
