package hub

import (
	"fmt"
	"slices"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/clock"
	"playerhub/server/internal/delivery"
	"playerhub/server/internal/interest"
	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
	"playerhub/server/internal/world"
)

type fixture struct {
	t       *testing.T
	clock   *clock.Fake
	hub     *Hub
	boxes   map[uint32]*delivery.Outbox
	session uint64
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

func (f *fixture) connectAs(displayName string) uint32 {
	f.session++
	slot, _ := f.connectSession(f.session, displayName)
	return slot
}

func (f *fixture) connectSession(session uint64, displayName string) (slot, replaced uint32) {
	box := delivery.NewOutbox(1024)
	slot, replaced = f.hub.ConnectClient(box, Identity{Session: session, DisplayName: displayName})
	f.boxes[slot] = box
	return slot, replaced
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
	c := &pb.Context{WorldspaceId: 60, GraphEpoch: epoch}
	if furniture {
		c.Furniture = &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1}
	}
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: c}}
}

func checkpoint(sequence uint64) *pb.ClientMessage {
	return checkpointAt(sequence, int32(sequence))
}

// checkpointAt places the player x world units along the X axis.
func checkpointAt(sequence uint64, x int32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{
		Sequence: sequence, X: x * wire.PositionScale, Flags: uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN),
		Time: uint32(sequence * 50 % wire.TimeModulus)}}}
}

func worldspace(epoch uint64, worldspace uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: worldspace, GraphEpoch: epoch}}}
}

func action(sequence, anchor uint64) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: &pb.Action{
		ActionSequence: sequence, GraphEpoch: 1, CheckpointSequence: anchor,
		Kind: &pb.Action_Attack{Attack: &pb.Attack{Action: pb.AttackAction_ATTACK_ACTION_NORMAL}}}}}
}

func shapeshift(epoch uint64, form pb.Shapeshift) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{
		WorldspaceId: 60, GraphEpoch: epoch, Shapeshift: form}}}
}

func beastAction(sequence, epoch, anchor uint64, beast pb.BeastAction) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: &pb.Action{
		ActionSequence: sequence, GraphEpoch: epoch, CheckpointSequence: anchor, Kind: &pb.Action_Beast{Beast: beast}}}}
}

func vampireLordCheckpoint(sequence uint64, flag pb.CheckpointFlag) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{
		Sequence: sequence, Flags: uint32(flag), Time: uint32(sequence * 50 % wire.TimeModulus)}}}
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

func whisper(id, recipient uint32, text string) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Chat{Chat: &pb.ChatSend{
		RequestId: id, Text: text, Channel: pb.ChatChannel_CHAT_CHANNEL_WHISPER, Recipient: recipient}}}
}

func vanish(session uint64, vanished bool) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Vanish{Vanish: &pb.Vanish{SessionId: session, Vanished: vanished}}}
}

func inspect(slot uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Inspect{Inspect: &pb.Inspect{Slot: slot}}}
}

func interior(epoch uint64, cell uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{InteriorCellId: cell, GraphEpoch: epoch}}}
}

func (f *fixture) place(slot uint32, sequence uint64) {
	f.send(slot, enter())
	f.send(slot, contextMessage(1, false))
	f.send(slot, checkpoint(sequence))
}

func (f *fixture) placeAt(slot uint32, sequence uint64, x int32) {
	f.send(slot, enter())
	f.send(slot, contextMessage(1, false))
	f.send(slot, checkpointAt(sequence, x))
}

func (f *fixture) messages(slot uint32) []*pb.ServerMessage {
	f.t.Helper()
	var b delivery.Batch
	if ok, _ := f.boxes[slot].Drain(&b, f.clock.Now()); !ok {
		f.t.Fatal("outbox closed")
	}
	var messages []*pb.ServerMessage
	err := b.Write(func(frame []byte) error {
		m := &pb.ServerMessage{}
		if err := proto.Unmarshal(frame, m); err != nil {
			return err
		}
		messages = append(messages, m)
		return nil
	})
	if err != nil {
		f.t.Fatal(err)
	}
	return messages
}

func (f *fixture) received(slot uint32) []string {
	f.t.Helper()
	var lines []string
	for _, m := range f.messages(slot) {
		lines = append(lines, describe(m))
	}
	return lines
}

func describe(m *pb.ServerMessage) string {
	switch body := m.Message.(type) {
	case *pb.ServerMessage_Welcome:
		return fmt.Sprintf("welcome %d", body.Welcome.Slot)
	case *pb.ServerMessage_ChatResult:
		return fmt.Sprintf("chat result %d %s", body.ChatResult.RequestId,
			strings.ToLower(strings.TrimPrefix(body.ChatResult.Rejection.String(), "CHAT_REJECTION_")))
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
			s += fmt.Sprintf(" appearance %d", e.Appearance.Revision)
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
		return fmt.Sprintf("appearance %d:%d", body.PeerAppearance.Slot, body.PeerAppearance.Appearance.Revision)
	case *pb.ServerMessage_PeerEquipment:
		return fmt.Sprintf("equipment %d:%d", body.PeerEquipment.Slot, body.PeerEquipment.Equipment.Revision)
	case *pb.ServerMessage_ChatMessage:
		c := body.ChatMessage
		channel := strings.ToLower(strings.TrimPrefix(c.Channel.String(), "CHAT_CHANNEL_"))
		s := fmt.Sprintf("chat %s %d %s", channel, c.Sender.Slot, c.Sender.Name)
		if c.Recipient != nil {
			s += fmt.Sprintf(" to %d %s", c.Recipient.Slot, c.Recipient.Name)
		}
		return s + ": " + c.Text
	case *pb.ServerMessage_Inspection:
		i := body.Inspection
		s := fmt.Sprintf("inspection %d level %d", i.Slot, i.Level)
		if i.Race != nil {
			s += fmt.Sprintf(" race %s|%X", i.Race.Plugin, i.Race.FormId)
		}
		if i.DefaultRace != nil {
			s += fmt.Sprintf(" default %s|%X", i.DefaultRace.Race.GetPlugin(), i.DefaultRace.Race.GetFormId())
		}
		return s
	case *pb.ServerMessage_TradeInvite:
		i := body.TradeInvite
		return fmt.Sprintf("trade invite %d %d>%d", i.TradeId, i.Sender.GetSlot(), i.Recipient.GetSlot())
	case *pb.ServerMessage_TradeOpen:
		return fmt.Sprintf("trade open %d", body.TradeOpen.TradeId)
	case *pb.ServerMessage_TradeOffer:
		o := body.TradeOffer
		return fmt.Sprintf("trade offer %d rev %d items %d", o.TradeId, o.Revision, len(o.Items))
	case *pb.ServerMessage_TradeReport:
		r := body.TradeReport
		s := fmt.Sprintf("trade report %d rev %d skipped", r.TradeId, r.Revision)
		for _, skip := range r.Skipped {
			s += fmt.Sprintf(" %d", skip.Index)
		}
		return s
	case *pb.ServerMessage_TradePluginQuery:
		q := body.TradePluginQuery
		return fmt.Sprintf("trade plugins %d query %s", q.TradeId, strings.Join(q.Plugins, " "))
	case *pb.ServerMessage_TradePluginReply:
		r := body.TradePluginReply
		return fmt.Sprintf("trade plugins %d missing %v", r.TradeId, r.Missing)
	case *pb.ServerMessage_TradeAccept:
		a := body.TradeAccept
		return fmt.Sprintf("trade accept %d own %d peer %d", a.TradeId, a.OwnRevision, a.PeerRevision)
	case *pb.ServerMessage_TradeCommit:
		c := body.TradeCommit
		return fmt.Sprintf("trade commit %d own %d peer %d", c.TradeId, c.OwnRevision, c.PeerRevision)
	case *pb.ServerMessage_TradeClose:
		c := body.TradeClose
		return fmt.Sprintf("trade close %d %s", c.TradeId, strings.TrimPrefix(c.Reason.String(), "TRADE_CLOSE_REASON_"))
	case *pb.ServerMessage_NearbyPlayers:
		s := "nearby"
		for _, slot := range slices.Sorted(slices.Values(body.NearbyPlayers.Joined)) {
			s += fmt.Sprintf(" +%d", slot)
		}
		for _, slot := range slices.Sorted(slices.Values(body.NearbyPlayers.Left)) {
			s += fmt.Sprintf(" -%d", slot)
		}
		return s
	case *pb.ServerMessage_Roster:
		s := "roster"
		for _, e := range body.Roster.Joined {
			s += fmt.Sprintf(" +%d %s", e.Slot, e.Name)
		}
		if body.Roster.Left != 0 {
			s += fmt.Sprintf(" -%d", body.Roster.Left)
		}
		return s
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
	expect(t, f.received(b), "enter 1 state 5 appearance 1 equipment 2")
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

func TestTicksWakeOnePhaseOfRecipients(t *testing.T) {
	f := newFixture(t)
	f.hub.usePhases(2)
	a, b, c, d := f.connect(), f.connect(), f.connect(), f.connect()
	for _, slot := range []uint32{a, b, c, d} {
		f.place(slot, 1)
	}
	for _, slot := range []uint32{b, c, d} {
		f.received(slot)
		signalled(f.boxes[slot])
	}
	f.send(a, checkpoint(2))
	f.hub.Tick()
	if !signalled(f.boxes[c]) || signalled(f.boxes[b]) || signalled(f.boxes[d]) {
		t.Fatal("the first tick should wake only the first phase")
	}
	f.hub.Tick()
	if !signalled(f.boxes[b]) || !signalled(f.boxes[d]) {
		t.Fatal("the second tick should wake the second phase")
	}
	for _, slot := range []uint32{b, c, d} {
		expect(t, f.received(slot), "states 1:2")
	}
}

func signalled(box *delivery.Outbox) bool {
	select {
	case <-box.Ready():
		return true
	default:
		return false
	}
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

func TestBeastActionsFollowTheForm(t *testing.T) {
	f := newFixture(t)
	a, b := f.connect(), f.connect()
	f.place(b, 1)
	f.place(a, 1)
	f.received(b)
	f.reject(a, beastAction(1, 1, 1, pb.BeastAction_BEAST_ACTION_WEREWOLF_HOWL_START), world.ErrIncompatibleStep)
	f.send(a, shapeshift(2, pb.Shapeshift_SHAPESHIFT_WEREBEAR))
	f.send(a, vampireLordCheckpoint(2, pb.CheckpointFlag_CHECKPOINT_FLAG_NONE))
	f.reject(a, action(1, 2), world.ErrIncompatibleStep)
	f.reject(a, beastAction(1, 2, 2, pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_BATS_START), world.ErrIncompatibleStep)
	f.send(a, beastAction(1, 2, 2, pb.BeastAction_BEAST_ACTION_WEREWOLF_HOWL_START))
	for _, flag := range []pb.CheckpointFlag{pb.CheckpointFlag_CHECKPOINT_FLAG_LEVITATING,
		pb.CheckpointFlag_CHECKPOINT_FLAG_MIST_FORM, pb.CheckpointFlag_CHECKPOINT_FLAG_BATS} {
		f.reject(a, vampireLordCheckpoint(3, flag), world.ErrIncompatibleStep)
	}
	f.send(a, shapeshift(3, pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD))
	f.send(a, vampireLordCheckpoint(3, pb.CheckpointFlag_CHECKPOINT_FLAG_LEVITATING|pb.CheckpointFlag_CHECKPOINT_FLAG_BATS))
	f.send(a, beastAction(2, 3, 3, pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_BATS_START))
	f.hub.Tick()
	expect(t, f.received(b), "context 1 epoch 2", "states 1:2", "action 1:1", "context 1 epoch 3", "states 1:3",
		"action 1:2")
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
	expect(t, f.received(b), "appearance 1:3", "leave 1 EXITED", "enter 1 state 1 appearance 4")
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
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	f.received(a)
	f.received(b)
	f.send(b, chat(7, "  hi  "))
	expect(t, f.received(b), "chat result 7 none", "chat global 2 Serana: hi")
	expect(t, f.received(a), "chat global 2 Serana: hi")
	f.reject(b, chat(8, "\x01"), validate.ErrChat)
	for i := range uint32(4) {
		f.send(b, chat(9+i, "again"))
	}
	f.reject(b, chat(20, "too many"), ErrRateLimited)
	f.clock.Advance(5.1)
	f.send(b, chat(21, "later"))
}

func TestNearbyChat(t *testing.T) {
	f := newFixture(t)
	a, b, c, d := f.connect(), f.connect(), f.connect(), f.connect()
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 8192)
	f.placeAt(c, 1, -8193)
	for _, slot := range []uint32{a, b, c, d} {
		f.received(slot)
	}
	nearby := func(id uint32, text string) *pb.ClientMessage {
		return chatOn(pb.ChatChannel_CHAT_CHANNEL_NEARBY, id, text)
	}
	f.send(a, nearby(7, "near"))
	expect(t, f.received(a), "chat result 7 none", "chat nearby 1 : near")
	expect(t, f.received(b), "chat nearby 1 : near")
	expect(t, f.received(c))
	expect(t, f.received(d))
	f.reject(d, nearby(8, "unplaced"), ErrNotPlaced)
	expect(t, f.received(d), "chat result 8 not_in_world")

	for _, slot := range []uint32{a, c} {
		f.send(slot, interior(2, 70))
	}
	f.send(a, checkpointAt(2, 0))
	f.send(c, checkpointAt(2, 50000))
	f.received(a)
	f.received(b)
	f.received(c)
	f.send(c, nearby(9, "echo"))
	expect(t, f.received(c), "chat result 9 none", "chat nearby 3 : echo")
	expect(t, f.received(a), "chat nearby 3 : echo")
	expect(t, f.received(b))
}

func TestNearbyPlayersFollowNearbyChat(t *testing.T) {
	f := newFixture(t)
	a, b, c, d := f.connectAs("Lydia"), f.connectAs("Serana"), f.connectAs("Aela"), f.connectAs("Brynjolf")
	v := f.connect()
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 8192)
	f.placeAt(c, 1, -8193)
	f.placeAt(v, 1, 10)
	nearby := func(slot uint32) []string {
		return slices.DeleteFunc(f.received(slot), func(line string) bool { return !strings.HasPrefix(line, "nearby") })
	}
	f.hub.UpdateInterest()
	expect(t, nearby(a), "nearby +2")
	expect(t, nearby(b), "nearby +1")
	expect(t, nearby(c))
	expect(t, nearby(d))

	f.send(b, vanish(1, true))
	f.send(c, checkpointAt(2, -8000))
	f.clock.Advance(0.9)
	f.hub.UpdateInterest()
	expect(t, nearby(a))
	f.clock.Advance(0.1)
	f.hub.UpdateInterest()
	expect(t, nearby(a), "nearby +3")
	expect(t, nearby(b))
	expect(t, nearby(c), "nearby +1")

	for _, slot := range []uint32{a, c} {
		f.send(slot, interior(2, 70))
	}
	f.send(a, checkpointAt(2, 0))
	f.send(c, checkpointAt(3, 50000))
	f.send(d, enter())
	f.send(d, interior(1, 70))
	f.send(d, checkpointAt(1, -50000))
	f.clock.Advance(1)
	f.hub.UpdateInterest()
	expect(t, nearby(a), "nearby +4 -2")
	expect(t, nearby(b), "nearby -1")
	expect(t, nearby(c), "nearby +4")
	expect(t, nearby(d), "nearby +1 +3")

	f.hub.Disconnect(c)
	f.clock.Advance(1)
	f.hub.UpdateInterest()
	expect(t, nearby(a))
	expect(t, nearby(d))
	f.send(a, exit())
	f.clock.Advance(1)
	f.hub.UpdateInterest()
	expect(t, nearby(a), "nearby -4")
	expect(t, nearby(d), "nearby -1")
}

func TestWhisper(t *testing.T) {
	f := newFixture(t)
	a, b, c := f.connectAs("Ulfric"), f.connectAs("Ulfric Stormcloak"), f.connectAs("Galmar")
	v := f.hub.Connect(world.KindReplay, nil)
	f.send(v, appearance(1, "Ghost"))
	for _, slot := range []uint32{a, b, c} {
		f.received(slot)
	}
	f.send(c, whisper(7, b, "  for Skyrim "))
	expect(t, f.received(c), "chat result 7 none", "chat whisper 3 Galmar to 2 Ulfric Stormcloak: for Skyrim")
	expect(t, f.received(b), "chat whisper 3 Galmar to 2 Ulfric Stormcloak: for Skyrim")
	expect(t, f.received(a))
	f.send(a, whisper(8, a, "note to self"))
	expect(t, f.received(a), "chat result 8 none", "chat whisper 1 Ulfric to 1 Ulfric: note to self")
	f.reject(a, whisper(9, 99, "hi"), ErrUnknownRecipient)
	f.reject(a, whisper(10, v, "boo"), ErrUnknownRecipient)
	f.reject(a, whisper(11, 0, "nobody"), validate.ErrChat)
	f.reject(a, whisper(12, b, " "), validate.ErrChat)
	f.reject(a, &pb.ClientMessage{Message: &pb.ClientMessage_Chat{Chat: &pb.ChatSend{RequestId: 13, Text: "hi", Recipient: b}}},
		validate.ErrChat)
	expect(t, f.received(a), "chat result 9 unknown_recipient", "chat result 10 unknown_recipient",
		"chat result 11 invalid", "chat result 12 invalid",
		"chat result 13 invalid")
	expect(t, f.received(b))

	f.hub.Disconnect(b)
	again := f.connectAs("Ulfric Stormcloak")
	f.received(a)
	f.clock.Advance(5.1)
	f.reject(a, whisper(14, b, "hi"), ErrUnknownRecipient)
	expect(t, f.received(again), fmt.Sprintf("welcome %d", again), "roster +1 Ulfric +3 Galmar +5 Ulfric Stormcloak")
}

func TestRoster(t *testing.T) {
	f := newFixture(t)
	a := f.connectAs("Lydia")
	expect(t, f.received(a), "welcome 1", "roster +1 Lydia")
	v := f.hub.Connect(world.KindReplay, nil)
	f.send(v, appearance(1, "Ghost"))
	b := f.connectAs("Serana")
	expect(t, f.received(b), "welcome 3", "roster +1 Lydia +3 Serana")
	expect(t, f.received(a), "roster +3 Serana")

	f.hub.Disconnect(v)
	expect(t, f.received(a))
	f.hub.Disconnect(b)
	expect(t, f.received(a), "roster -3")
	f.hub.Disconnect(b)
	expect(t, f.received(a))

	c, replaced := f.connectSession(1, "Lydia")
	if replaced != a {
		t.Fatalf("replaced %d", replaced)
	}
	expect(t, f.received(c), "welcome 4", "roster +4 Lydia")
	d := f.connectAs("Aela")
	expect(t, f.received(d), "welcome 5", "roster +4 Lydia +5 Aela")
	expect(t, f.received(c), "roster +5 Aela")
}

func TestAppearanceCarriesTheSessionDisplayName(t *testing.T) {
	f := newFixture(t)
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	f.place(b, 1)
	f.send(a, appearance(7, "Impostor"))
	f.place(a, 1)
	f.reject(a, appearance(7, "Lydia"), world.ErrStaleRevision)
	welcome := f.messages(a)[0].GetWelcome()
	if welcome.GetSlot() != a || welcome.GetSessionId() != 1 || welcome.GetDisplayName() != "Lydia" || welcome.GetSessionToken() != nil {
		t.Fatalf("welcome: %v", welcome)
	}
	messages := f.messages(b)
	enter := messages[len(messages)-1].GetPeerEnter()
	if enter.GetAppearance().GetName() != "Lydia" || enter.GetAppearance().GetRevision() != 7 {
		t.Fatalf("enter: %v", enter)
	}

	v := f.hub.Connect(world.KindReplay, nil)
	f.send(v, appearance(1, "Lydia"))
	f.place(v, 1)
	messages = f.messages(b)
	if enter := messages[len(messages)-1].GetPeerEnter(); enter.GetSlot() != v || enter.GetAppearance().GetName() != "Lydia" {
		t.Fatalf("virtual enter: %v", enter)
	}
	if f.hub.Name(a) != "Lydia" || f.hub.Name(v) != "Lydia" || f.hub.Name(99) != "" {
		t.Fatalf("names: %q %q %q", f.hub.Name(a), f.hub.Name(v), f.hub.Name(99))
	}
}

func TestSessionConnectingAgainReplacesItsConnection(t *testing.T) {
	f := newFixture(t)
	first, replaced := f.connectSession(1, "Lydia")
	if replaced != 0 {
		t.Fatalf("the first connection replaced %d", replaced)
	}
	f.session = 1
	b := f.connectAs("Serana")
	f.send(first, appearance(1, ""))
	f.place(first, 1)
	f.place(b, 1)
	f.received(b)

	second, replaced := f.connectSession(1, "Lydia")
	if replaced != first || second == first {
		t.Fatalf("connected as %d, replacing %d", second, replaced)
	}
	expect(t, f.received(b), "leave 1 DISCONNECTED", "roster -1", "roster +3 Lydia")
	f.reject(first, enter(), ErrUnknownPlayer)

	f.send(b, whisper(1, second, "hi"))
	expect(t, f.received(second), fmt.Sprintf("welcome %d", second), "roster +2 Serana +3 Lydia",
		"chat whisper 2 Serana to 3 Lydia: hi")

	f.hub.Disconnect(first)
	f.send(b, whisper(2, second, "still there"))
	expect(t, f.received(second), "chat whisper 2 Serana to 3 Lydia: still there")
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

func TestRosterAndChatCarrySessions(t *testing.T) {
	f := newFixture(t)
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	if joined := f.messages(a)[2].GetRoster().GetJoined(); len(joined) != 1 || joined[0].GetSessionId() != 2 {
		t.Fatalf("join: %v", joined)
	}
	roster := f.messages(b)[1].GetRoster().GetJoined()
	if len(roster) != 2 || roster[0].GetSessionId() != 1 || roster[1].GetSessionId() != 2 {
		t.Fatalf("roster: %v", roster)
	}
	f.send(a, whisper(1, b, "hi"))
	line := f.messages(b)[0].GetChatMessage()
	if line.GetSender().GetSessionId() != 1 || line.GetRecipient().GetSessionId() != 2 {
		t.Fatalf("whisper: %v", line)
	}
}

func TestVanishFollowsTheSessionAtOnce(t *testing.T) {
	f := newFixture(t)
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 100)
	f.received(a)
	f.received(b)

	f.send(b, vanish(1, true))
	expect(t, f.received(b), "leave 1 EXCLUDED")
	f.send(b, checkpointAt(2, 120))
	f.send(a, checkpointAt(2, 20))
	f.hub.Tick()
	f.hub.UpdateInterest()
	expect(t, f.received(a), "nearby +2", "states 2:2")
	expect(t, f.received(b), "nearby +1")
	f.send(b, vanish(1, true))

	again, _ := f.connectSession(1, "Lydia")
	f.placeAt(again, 1, 0)
	f.hub.UpdateInterest()
	expect(t, f.received(b), "roster -1", "roster +3 Lydia")
	expect(t, f.received(again), "welcome 3", "roster +2 Serana +3 Lydia", "enter 2 state 2", "nearby +2")

	f.send(b, vanish(1, false))
	expect(t, f.received(b), "enter 3 state 1")
	f.send(b, vanish(1, false))
	f.send(b, vanish(99, true))
	expect(t, f.received(b))

	c, _ := f.connectSession(4, "Aela")
	f.hub.SetVanished(c, []uint64{1})
	f.placeAt(c, 1, 50)
	f.hub.UpdateInterest()
	expect(t, f.received(c), "welcome 4", "roster +2 Serana +3 Lydia +4 Aela", "enter 2 state 2", "nearby +2 +3")
}

func TestVanishedSessionsAreBounded(t *testing.T) {
	f := newFixture(t)
	a := f.connectAs("Lydia")
	for session := range uint64(validate.MaximumVanished) {
		f.send(a, vanish(session+100, true))
	}
	f.reject(a, vanish(1, true), ErrTooManyVanished)
	f.send(a, vanish(100, false))
	f.send(a, vanish(1, true))
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
	f.send(a, contextMessage(1, true))
	f.send(a, checkpointAt(5, 10))
	f.hub.Tick()
	expect(t, f.received(b), "context 1 epoch 1", "states 1:5")
	f.send(a, checkpointAt(6, 11))
	f.hub.Tick()
	expect(t, f.received(b), "states 1:6")
	// Sub-unit drift is suppressed as whole-unit checkpoints used to round it away.
	f.send(a, quarterCheckpoint(7, 11*wire.PositionScale+1))
	f.hub.Tick()
	expect(t, f.received(b))
	f.send(a, quarterCheckpoint(8, 11*wire.PositionScale+2))
	f.hub.Tick()
	expect(t, f.received(b), "states 1:8")
}

func quarterCheckpoint(sequence uint64, x int32) *pb.ClientMessage {
	message := checkpointAt(sequence, 0)
	message.GetCheckpoint().X = x
	return message
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
	expect(t, f.received(viewer), "welcome 1", "enter 4 state 1", "enter 3 state 1")
	f.clock.Advance(1.5)
	f.hub.UpdateInterest()
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

func TestInspect(t *testing.T) {
	f := newFixture(t)
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	f.send(b, &pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
		Revision: 1, Name: "Serana", Level: 31, Race: &pb.FormRef{Plugin: "Mod.esp", FormId: 0x812},
		DefaultRace: &pb.DefaultRace{Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0x13746}, Female: true}}}})
	v := f.hub.Connect(world.KindReplay, nil)
	f.send(v, appearance(1, "Lydia"))
	f.received(a)
	f.received(b)

	f.send(a, inspect(b))
	f.send(a, inspect(a))
	f.send(a, inspect(v))
	f.send(a, inspect(99))
	expect(t, f.received(a), "inspection 2 level 31 race Mod.esp|812 default Skyrim.esm|13746",
		"inspection 1 level 0", "inspection 3 level 0", "inspection 99 level 0")
	expect(t, f.received(b))

	f.send(a, inspect(b))
	f.reject(a, inspect(b), ErrRateLimited)
	f.clock.Advance(0.5)
	f.send(a, inspect(b))
}
