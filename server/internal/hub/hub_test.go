package hub

import (
	"fmt"
	"slices"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/clock"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/interest"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire/pb"
	"skyrim-mp/server/internal/world"
)

type fixture struct {
	t     *testing.T
	clock *clock.Fake
	hub   *Hub
	boxes map[uint32]*delivery.Outbox
}

func newFixture(t *testing.T) *fixture {
	c := clock.NewFake()
	return &fixture{t: t, clock: c, hub: New(Options{Clock: c}), boxes: make(map[uint32]*delivery.Outbox)}
}

func (f *fixture) connect() uint32 {
	box := delivery.NewOutbox(1024)
	slot := f.hub.Connect(world.KindClient, box)
	f.boxes[slot] = box
	return slot
}

func (f *fixture) send(slot uint32, m *pb.ClientMessage) {
	f.t.Helper()
	if err := f.hub.Handle(slot, m); err != nil {
		f.t.Fatalf("slot %d: %v", slot, err)
	}
}

func (f *fixture) reject(slot uint32, m *pb.ClientMessage, want error) {
	f.t.Helper()
	if err := f.hub.Handle(slot, m); err != want {
		f.t.Fatalf("slot %d: got %v, want %v", slot, err, want)
	}
}

func enter() *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}}
}
func exit() *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}}
}

func contextMessage(epoch uint64, furniture bool) *pb.ClientMessage {
	c := &pb.Context{CellId: 60, WorldspaceId: 60, GraphEpoch: epoch}
	if furniture {
		c.Furniture = &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1}
	}
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: c}}
}

func checkpoint(sequence uint64) *pb.ClientMessage {
	return checkpointAt(sequence, int32(sequence))
}

func checkpointAt(sequence uint64, x int32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{
		Sequence: sequence, X: x, Flags: uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN)}}}
}

func worldspace(epoch uint64, worldspace uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: worldspace, GraphEpoch: epoch}}}
}

func action(sequence, anchor uint64) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: &pb.Action{
		ActionSequence: sequence, GraphEpoch: 1, CheckpointSequence: anchor, Action: pb.AttackAction_ATTACK_ACTION_NORMAL}}}
}

func appearance(revision uint64, name string) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{Revision: revision, Name: name}}}
}

func equipment(revision uint64) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Equipment{Equipment: &pb.Equipment{Revision: revision}}}
}

func chat(id uint32, text string) *pb.ClientMessage {
	return chatOn(pb.ChatChannel_CHAT_CHANNEL_GLOBAL, id, text)
}

func chatOn(channel pb.ChatChannel, id uint32, text string) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Chat{Chat: &pb.ChatSend{RequestId: id, Text: text, Channel: channel}}}
}

func interior(epoch uint64, cell uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{CellId: cell, GraphEpoch: epoch}}}
}

// place puts the player in the world with a context and a checkpoint.
func (f *fixture) place(slot uint32, sequence uint64) {
	f.send(slot, enter())
	f.send(slot, contextMessage(1, false))
	f.send(slot, checkpoint(sequence))
}

// placeAt puts the player in the world in worldspace 60 at x.
func (f *fixture) placeAt(slot uint32, sequence uint64, x int32) {
	f.send(slot, enter())
	f.send(slot, contextMessage(1, false))
	f.send(slot, checkpointAt(sequence, x))
}

// received drains the recipient's outbox and describes each message in one
// line.
func (f *fixture) received(slot uint32) []string {
	f.t.Helper()
	var b delivery.Batch
	if ok, _ := f.boxes[slot].Drain(&b, f.clock.Now()); !ok {
		f.t.Fatal("outbox closed")
	}
	var lines []string
	err := b.Write(func(frame []byte) error {
		var m pb.ServerMessage
		if err := proto.Unmarshal(frame, &m); err != nil {
			return err
		}
		lines = append(lines, describe(&m))
		return nil
	})
	if err != nil {
		f.t.Fatal(err)
	}
	return lines
}

func describe(m *pb.ServerMessage) string {
	switch body := m.Message.(type) {
	case *pb.ServerMessage_Welcome:
		return fmt.Sprintf("welcome %d", body.Welcome.Slot)
	case *pb.ServerMessage_Result:
		return fmt.Sprintf("result %d %v %s", body.Result.RequestId, body.Result.Ok, body.Result.Error)
	case *pb.ServerMessage_PeerStates:
		var states []string
		for _, s := range body.PeerStates.States {
			states = append(states, fmt.Sprintf("%d:%d", s.Player, s.Sequence))
		}
		slices.Sort(states)
		return "states " + strings.Join(states, " ")
	case *pb.ServerMessage_PeerEnter:
		e := body.PeerEnter
		s := fmt.Sprintf("enter %d state %d", e.Slot, e.State.GetSequence())
		if e.Appearance != nil {
			s += fmt.Sprintf(" appearance %d #%d", e.Appearance.Revision, e.NameIndex)
		}
		if e.Equipment != nil {
			s += fmt.Sprintf(" equipment %d", e.Equipment.Revision)
		}
		return s
	case *pb.ServerMessage_PeerContext:
		return fmt.Sprintf("context %d epoch %d", body.PeerContext.Slot, body.PeerContext.Context.GraphEpoch)
	case *pb.ServerMessage_PeerLeave:
		return fmt.Sprintf("leave %d %s", body.PeerLeave.Slot, strings.TrimPrefix(body.PeerLeave.Reason.String(), "LEAVE_REASON_"))
	case *pb.ServerMessage_PeerAction:
		return fmt.Sprintf("action %d:%d", body.PeerAction.Slot, body.PeerAction.Action.ActionSequence)
	case *pb.ServerMessage_PeerAppearance:
		return fmt.Sprintf("appearance %d:%d #%d", body.PeerAppearance.Slot, body.PeerAppearance.Appearance.Revision, body.PeerAppearance.NameIndex)
	case *pb.ServerMessage_PeerEquipment:
		return fmt.Sprintf("equipment %d:%d", body.PeerEquipment.Slot, body.PeerEquipment.Equipment.Revision)
	case *pb.ServerMessage_ChatMessage:
		c := body.ChatMessage
		channel := strings.ToLower(strings.TrimPrefix(c.Channel.String(), "CHAT_CHANNEL_"))
		s := fmt.Sprintf("chat %s %d %s#%d", channel, c.Sender.Slot, c.Sender.Name, c.Sender.NameIndex)
		if c.Recipient != nil {
			s += fmt.Sprintf(" to %d %s#%d", c.Recipient.Slot, c.Recipient.Name, c.Recipient.NameIndex)
		}
		return s + ": " + c.Text
	}
	return "unknown"
}

func expect(t *testing.T, got []string, want ...string) {
	t.Helper()
	if !slices.Equal(got, want) {
		t.Fatalf("got\n  %s\nwant\n  %s", strings.Join(got, "\n  "), strings.Join(want, "\n  "))
	}
}

func TestWelcomeComesFirst(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	expect(t, f.received(a), fmt.Sprintf("welcome %d", a))
}

func TestPeerEnterCarriesPresentationThenState(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(b, 1)
	f.received(a)
	f.received(b)
	f.send(a, appearance(1, "Lydia"))
	f.send(a, equipment(2))
	f.send(a, enter())
	f.send(a, contextMessage(1, false))
	expect(t, f.received(b))
	f.send(a, checkpoint(5))
	expect(t, f.received(b), "enter 1 state 5 appearance 1 #1 equipment 2")
	expect(t, f.received(a), "enter 2 state 1")
}

func TestUnplacedViewerSeesNobody(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(a, 1)
	expect(t, f.received(b), "welcome 2")
	f.send(b, enter())
	f.send(b, contextMessage(1, false))
	expect(t, f.received(b))
	f.send(b, checkpoint(1))
	expect(t, f.received(b), "enter 1 state 1")
	f.send(b, exit())
	expect(t, f.received(b), "leave 1 VIEWER_UNPLACED")
	expect(t, f.received(a), "welcome 1", "enter 2 state 1", "leave 2 EXITED")
}

func TestPausedPlayerIsSeenByANewcomer(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	f.place(a, 9)
	f.hub.Tick()
	// A paused game publishes nothing; the newcomer still gets the stored state.
	f.clock.Advance(60)
	b := f.connect()
	f.place(b, 1)
	expect(t, f.received(b), "welcome 2", "enter 1 state 9")
}

func TestCheckpointsAreBatchedPerTick(t *testing.T) {
	f := newFixture(t)
	a, b, c := f.connect(), f.connect(), f.connect()
	f.place(c, 1)
	f.place(a, 1)
	f.place(b, 1)
	f.received(c)
	f.send(a, checkpoint(2))
	f.send(a, checkpoint(3))
	f.send(b, checkpoint(2))
	expect(t, f.received(c))
	f.hub.Tick()
	expect(t, f.received(c), "states 1:3 2:2")
}

func TestActionIsPrecededByItsCheckpoint(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(b, 1)
	f.place(a, 1)
	f.received(b)
	f.send(a, checkpoint(2))
	f.send(a, action(1, 2))
	f.send(a, checkpoint(3))
	f.hub.Tick()
	expect(t, f.received(b), "states 1:2", "action 1:1", "states 1:3")
}

func TestActionValidation(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	f.reject(a, action(1, 1), world.ErrIncompatibleStep)
	f.place(a, 4)
	f.reject(a, action(1, 5), world.ErrIncompatibleStep)
	f.send(a, action(7, 4))
	f.reject(a, action(9, 4), world.ErrActionGap)
	f.send(a, action(8, 4))
	f.send(a, contextMessage(1, true))
	f.reject(a, action(9, 4), world.ErrIncompatibleStep)
}

func TestActionsAreRateLimited(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	f.place(a, 1)
	for i := range uint64(10) {
		f.send(a, action(i+1, 1))
	}
	f.reject(a, action(11, 1), ErrRateLimited)
	f.clock.Advance(0.1)
	f.send(a, action(11, 1))
}

func TestContextChangeKeepsOrder(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(b, 1)
	f.place(a, 1)
	f.received(b)
	f.send(a, checkpoint(2))
	f.send(a, contextMessage(2, false))
	f.send(a, checkpoint(3))
	f.send(a, contextMessage(2, false))
	f.hub.Tick()
	expect(t, f.received(b), "states 1:2", "context 1 epoch 2", "states 1:3")
	f.reject(a, contextMessage(1, false), world.ErrStaleGraphEpoch)
}

func TestCheckpointRules(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	f.reject(a, checkpoint(1), world.ErrNotInWorld)
	f.send(a, enter())
	f.reject(a, checkpoint(1), world.ErrNoContext)
	f.send(a, contextMessage(1, false))
	f.send(a, checkpoint(5))
	f.reject(a, checkpoint(5), world.ErrStaleSequence)
	f.send(a, exit())
	f.send(a, enter())
	f.send(a, contextMessage(1, false))
	// Exit forgets the stream, so a lower sequence starts it again.
	f.send(a, checkpoint(1))
}

func TestExitAndReenterKeepPresentation(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(b, 1)
	f.send(a, appearance(1, "Lydia"))
	f.place(a, 1)
	f.received(b)
	f.send(a, checkpoint(2))
	f.send(a, exit())
	f.hub.Tick()
	expect(t, f.received(b), "leave 1 EXITED")
	f.place(a, 1)
	expect(t, f.received(b), "enter 1 state 1")
	f.send(a, appearance(2, "Lydia"))
	f.send(a, appearance(3, "Lydia"))
	f.send(a, exit())
	f.send(a, appearance(4, "Lydia"))
	f.place(a, 1)
	expect(t, f.received(b), "appearance 1:3 #1", "leave 1 EXITED", "enter 1 state 1 appearance 4 #1")
}

func TestDisconnectReachesHoldersOfPresentation(t *testing.T) {
	f := newFixture(t)
	a, b, c := f.connect(), f.connect(), f.connect()
	f.place(b, 1)
	f.send(a, appearance(1, "Lydia"))
	f.place(a, 1)
	f.send(a, exit())
	f.received(b)
	f.received(c)
	d := f.connect()
	f.hub.Disconnect(a)
	expect(t, f.received(b), "leave 1 DISCONNECTED")
	// c was never placed, so it never held a's presentation.
	expect(t, f.received(c))
	expect(t, f.received(d), "welcome 4")
}

func TestVirtualPlayerIsSeenButReceivesNothing(t *testing.T) {
	f := newFixture(t)
	a := f.connect()
	v := f.hub.Connect(world.KindReplay, nil)
	f.place(v, 3)
	f.place(a, 1)
	expect(t, f.received(a), "welcome 1", "enter 2 state 3")
}

func TestChat(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.send(a, appearance(1, "Lydia"))
	f.send(b, appearance(1, "lydia"))
	f.received(a)
	f.received(b)
	f.send(b, chat(7, "  hi  "))
	expect(t, f.received(b), "result 7 true ", "chat global 2 lydia#2: hi")
	expect(t, f.received(a), "chat global 2 lydia#2: hi")
	f.reject(b, chat(8, "\x01"), validate.ErrChat)
	for i := range uint32(4) {
		f.send(b, chat(9+i, "again"))
	}
	f.reject(b, chat(20, "too many"), ErrRateLimited)
	f.clock.Advance(5.1)
	f.send(b, chat(21, "later"))
}

func TestLocalChat(t *testing.T) {
	f := newFixture(t)
	a, b, c, d := f.connect(), f.connect(), f.connect(), f.connect()
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 8192)
	f.placeAt(c, 1, -8193)
	for _, slot := range []uint32{a, b, c, d} {
		f.received(slot)
	}
	local := func(id uint32, text string) *pb.ClientMessage {
		return chatOn(pb.ChatChannel_CHAT_CHANNEL_LOCAL, id, text)
	}
	f.send(a, local(7, "near"))
	expect(t, f.received(a), "result 7 true ", "chat local 1 #1: near")
	expect(t, f.received(b), "chat local 1 #1: near")
	expect(t, f.received(c))
	expect(t, f.received(d))
	f.reject(d, local(8, "unplaced"), ErrNotPlaced)
	expect(t, f.received(d), "result 8 false not in world")

	// An interior is heard whole; another space is not.
	for _, slot := range []uint32{a, c} {
		f.send(slot, interior(2, 70))
	}
	f.send(a, checkpointAt(2, 0))
	f.send(c, checkpointAt(2, 50000))
	f.received(a)
	f.received(b)
	f.received(c)
	f.send(c, local(9, "echo"))
	expect(t, f.received(c), "result 9 true ", "chat local 3 #1: echo")
	expect(t, f.received(a), "chat local 3 #1: echo")
	expect(t, f.received(b))
}

func TestWhisper(t *testing.T) {
	f := newFixture(t)
	a, b, c := f.connect(), f.connect(), f.connect()
	f.send(a, appearance(1, "Ulfric"))
	f.send(b, appearance(1, "Ulfric Stormcloak"))
	f.send(c, appearance(1, "ulfric"))
	v := f.hub.Connect(world.KindReplay, nil)
	f.send(v, appearance(1, "Ghost"))
	for _, slot := range []uint32{a, b, c} {
		f.received(slot)
	}
	whisper := func(id uint32, text string) *pb.ClientMessage {
		return chatOn(pb.ChatChannel_CHAT_CHANNEL_WHISPER, id, text)
	}
	f.send(c, whisper(7, "ulfric stormcloak  for Skyrim"))
	expect(t, f.received(c), "result 7 true ", "chat whisper 3 ulfric#2 to 2 Ulfric Stormcloak#1: for Skyrim")
	expect(t, f.received(b), "chat whisper 3 ulfric#2 to 2 Ulfric Stormcloak#1: for Skyrim")
	expect(t, f.received(a))
	f.send(a, whisper(8, "Ulfric #2 hi"))
	expect(t, f.received(a), "result 8 true ", "chat whisper 1 Ulfric#1 to 3 ulfric#2: hi")
	expect(t, f.received(c), "chat whisper 1 Ulfric#1 to 3 ulfric#2: hi")
	f.send(a, whisper(9, "Ulfric note to self"))
	expect(t, f.received(a), "result 9 true ", "chat whisper 1 Ulfric#1 to 1 Ulfric#1: note to self")
	f.reject(a, whisper(10, "Ulfric #3 hi"), ErrUnknownRecipient)
	f.reject(a, whisper(11, "Ghost boo"), ErrUnknownRecipient)
	f.reject(a, whisper(12, "Ulfric Stormcloak"), ErrEmptyWhisper)
	expect(t, f.received(a), "result 10 false unknown recipient", "result 11 false unknown recipient", "result 12 false empty whisper")
	f.hub.Disconnect(b)
	f.received(a)
	f.send(a, whisper(13, "Ulfric Stormcloak hi"))
	expect(t, f.received(a), "result 13 true ", "chat whisper 1 Ulfric#1 to 1 Ulfric#1: Stormcloak hi")
}

func TestRangeHysteresisAndDwell(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.placeAt(b, 1, 0)
	f.placeAt(a, 1, 9000)
	f.received(a)
	expect(t, f.received(b), "welcome 2")
	f.send(a, checkpointAt(2, 8000))
	f.hub.UpdateInterest()
	expect(t, f.received(b), "enter 1 state 2")
	f.send(a, checkpointAt(3, 9500))
	f.hub.UpdateInterest()
	f.send(a, checkpointAt(4, 20000))
	f.hub.UpdateInterest()
	f.hub.Tick()
	// Still within the minimum time in view.
	expect(t, f.received(b), "states 1:4")
	f.clock.Advance(1.1)
	f.hub.UpdateInterest()
	expect(t, f.received(b), "leave 1 OUT_OF_RANGE")
	expect(t, f.received(a), "enter 2 state 1", "leave 2 OUT_OF_RANGE")
}

func TestExclusionsTakeEffectAtOnce(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 100)
	f.received(a)
	f.received(b)
	f.hub.SetHidden(a, true)
	expect(t, f.received(b), "leave 1 EXCLUDED")
	expect(t, f.received(a))
	f.hub.UpdateInterest()
	expect(t, f.received(b))
	f.hub.SetHidden(a, false)
	expect(t, f.received(b), "enter 1 state 1")
	f.hub.SetExcluded(b, a, true)
	expect(t, f.received(b), "leave 1 EXCLUDED")
	expect(t, f.received(a))
	if len(f.hub.Exclusions()) != 1 || !f.hub.SetExcluded(b, a, false) {
		t.Fatal("exclusion not listed or not lifted")
	}
	expect(t, f.received(b), "enter 1 state 1")
	f.hub.SetExcluded(b, a, true)
	f.hub.Disconnect(a)
	if len(f.hub.Exclusions()) != 0 {
		t.Fatal("a disconnect kept its exclusions")
	}
}

func TestSpaceChangeLeavesAtOnceAndPlacesAgain(t *testing.T) {
	f := newFixture(t)
	a, b, c := f.connect(), f.connect(), f.connect()
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 100)
	f.send(c, enter())
	f.send(c, worldspace(1, 61))
	f.send(c, checkpointAt(1, 0))
	f.received(a)
	f.received(b)
	f.received(c)
	f.send(a, worldspace(1, 61))
	expect(t, f.received(b), "leave 1 OUT_OF_RANGE")
	expect(t, f.received(a), "leave 2 VIEWER_UNPLACED")
	expect(t, f.received(c))
	f.send(a, checkpointAt(2, 50))
	expect(t, f.received(c), "enter 1 state 2")
	expect(t, f.received(a), "enter 3 state 1")
	f.reject(a, checkpointAt(2, 60), world.ErrStaleSequence)
}

func TestIdleCheckpointsAreSuppressedButAnchorsAreNot(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.placeAt(b, 1, 0)
	f.placeAt(a, 1, 10)
	f.received(b)
	f.send(a, checkpointAt(2, 10))
	f.send(a, checkpointAt(3, 10))
	f.hub.Tick()
	expect(t, f.received(b))
	f.send(a, action(1, 3))
	expect(t, f.received(b), "states 1:3", "action 1:1")
	f.send(a, checkpointAt(4, 10))
	f.hub.Tick()
	expect(t, f.received(b))
	// A context change is applied by clients with the next checkpoint.
	f.send(a, contextMessage(1, true))
	f.send(a, checkpointAt(5, 10))
	f.hub.Tick()
	expect(t, f.received(b), "context 1 epoch 1", "states 1:5")
	f.send(a, checkpointAt(6, 11))
	f.hub.Tick()
	expect(t, f.received(b), "states 1:6")
}

func newFixtureWith(t *testing.T, change func(*interest.Config)) *fixture {
	c := clock.NewFake()
	config := interest.DefaultConfig()
	change(&config)
	return &fixture{t: t, clock: c, hub: New(Options{Clock: c, Interest: config}), boxes: make(map[uint32]*delivery.Outbox)}
}

func TestCapKeepsTheNearestWithHysteresis(t *testing.T) {
	f := newFixtureWith(t, func(c *interest.Config) { c.MaxVisible = 2 })
	viewer := f.connect()
	f.placeAt(viewer, 1, 0)
	near, middle, far := f.connect(), f.connect(), f.connect()
	f.placeAt(far, 1, 3000)
	f.placeAt(middle, 1, 2000)
	f.placeAt(near, 1, 1000)
	f.hub.UpdateInterest()
	// The first two fill the view at once; the third waits for the pass, and
	// the dwell keeps both incumbents meanwhile.
	expect(t, f.received(viewer), "welcome 1", "enter 4 state 1", "enter 3 state 1")
	f.clock.Advance(1.5)
	f.hub.UpdateInterest()
	// The newcomer at 1000 is not 1024 nearer than the incumbent at 2000 - 1024,
	// but it is than the one at 3000.
	expect(t, f.received(viewer), "leave 4 OUT_OF_RANGE", "enter 2 state 1")
	f.send(far, checkpointAt(2, 1500))
	f.clock.Advance(1.5)
	f.hub.UpdateInterest()
	expect(t, f.received(viewer))
	f.send(far, checkpointAt(3, 100))
	f.hub.UpdateInterest()
	expect(t, f.received(viewer), "leave 3 OUT_OF_RANGE", "enter 4 state 3")
}

func TestFarPeersAreWrittenAtTheirRate(t *testing.T) {
	f := newFixture(t)
	viewer, peer := f.connect(), f.connect()
	f.placeAt(viewer, 1, 0)
	f.placeAt(peer, 1, 6000)
	f.received(viewer)
	f.send(peer, checkpointAt(2, 6001))
	f.hub.Tick()
	expect(t, f.received(viewer), "states 2:2")
	f.clock.Advance(0.05)
	f.send(peer, checkpointAt(3, 6002))
	f.hub.Tick()
	expect(t, f.received(viewer))
	f.clock.Advance(0.06)
	expect(t, f.received(viewer), "states 2:3")
	f.send(peer, checkpointAt(4, 3000))
	f.hub.UpdateInterest()
	f.hub.Tick()
	expect(t, f.received(viewer), "states 2:4")
	f.send(peer, checkpointAt(5, 3001))
	f.hub.Tick()
	expect(t, f.received(viewer), "states 2:5")
}

func TestEntersAreSpreadNearestFirst(t *testing.T) {
	c := clock.NewFake()
	f := &fixture{t: t, clock: c, boxes: make(map[uint32]*delivery.Outbox),
		hub: New(Options{Clock: c, EntersPerSecond: 2})}
	var peers []uint32
	for i := range 4 {
		p := f.connect()
		f.placeAt(p, 1, int32(1000*(4-i)))
		peers = append(peers, p)
	}
	viewer := f.connect()
	f.placeAt(viewer, 1, 0)
	expect(t, f.received(viewer), "welcome 5", "enter 4 state 1", "enter 3 state 1")
	f.hub.UpdateInterest()
	expect(t, f.received(viewer))
	c.Advance(1)
	f.hub.UpdateInterest()
	expect(t, f.received(viewer), "enter 2 state 1", "enter 1 state 1")
}
