// Package hub owns all world state. One goroutine calls its methods: sessions,
// playback, and the admin API submit work to it through Loop, and tests call
// the methods directly with a fake clock.
package hub

import (
	"errors"
	"log/slog"
	"slices"

	"skyrim-mp/server/internal/clock"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/limit"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
	"skyrim-mp/server/internal/world"
)

var (
	ErrUnknownPlayer = errors.New("unknown player")
	ErrRateLimited   = errors.New("rate limited")
)

// Recorder receives every accepted client message of every player, so it can
// append those of captured players.
type Recorder interface {
	Record(slot uint32, m *pb.ClientMessage)
	// Stop ends a capture of a player that is gone.
	Stop(slot uint32, reason string)
}

type Options struct {
	Clock            clock.Clock
	ActionsPerSecond float64
	ActionBurst      float64
	Recorder         Recorder
	Logger           *slog.Logger
	// Called whenever something the admin panel shows changes.
	OnChange func()
}

type member struct {
	*world.Player
	// Nil for a virtual player, which receives nothing.
	outbox *delivery.Outbox

	// Recipients subscribed to this player.
	subscribers []*member
	// Peers this player receives, and the presentation revisions it holds.
	subscribed     map[uint32]struct{}
	heldAppearance map[uint32]uint64
	heldEquipment  map[uint32]uint64

	// A checkpoint arrived since the last tick.
	changed bool
	// The outbox received checkpoints this tick.
	touched bool

	actions limit.TokenBucket
	chat    chatThrottle
}

type Hub struct {
	opts       Options
	members    map[uint32]*member
	recipients []*member
	nextSlot   uint32
	names      *world.Names
	changed    []*member
	touched    []*member
}

func New(opts Options) *Hub {
	if opts.Clock == nil {
		opts.Clock = clock.Real{}
	}
	if opts.Logger == nil {
		opts.Logger = slog.Default()
	}
	if opts.OnChange == nil {
		opts.OnChange = func() {}
	}
	if opts.ActionsPerSecond == 0 {
		opts.ActionsPerSecond, opts.ActionBurst = 10, 10
	}
	return &Hub{opts: opts, members: make(map[uint32]*member), names: world.NewNames()}
}

// Connect adds a player and returns its slot. A player with an outbox is
// welcomed and receives every placed peer.
func (h *Hub) Connect(kind world.Kind, outbox *delivery.Outbox) uint32 {
	h.nextSlot++
	m := &member{
		Player:         &world.Player{Slot: h.nextSlot, Kind: kind, ConnectedAt: h.opts.Clock.Now()},
		outbox:         outbox,
		subscribed:     make(map[uint32]struct{}),
		heldAppearance: make(map[uint32]uint64),
		heldEquipment:  make(map[uint32]uint64),
	}
	h.members[m.Slot] = m
	if outbox != nil {
		h.recipients = append(h.recipients, m)
		outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Welcome{
			Welcome: &pb.Welcome{Slot: m.Slot}}}))
		for _, peer := range h.members {
			if h.visible(m, peer) {
				h.subscribe(m, peer)
			}
		}
	}
	h.opts.OnChange()
	return m.Slot
}

// Disconnect removes a player. Every recipient that receives it or holds its
// presentation is told to forget it.
func (h *Hub) Disconnect(slot uint32) {
	m, ok := h.members[slot]
	if !ok {
		return
	}
	if h.opts.Recorder != nil {
		h.opts.Recorder.Stop(slot, "disconnected")
	}
	for peer := range m.subscribed {
		h.members[peer].subscribers = remove(h.members[peer].subscribers, m)
	}
	leave := leaveMessage(slot, pb.LeaveReason_LEAVE_REASON_DISCONNECTED)
	for _, r := range h.recipients {
		_, subscribed := r.subscribed[slot]
		_, holdsAppearance := r.heldAppearance[slot]
		_, holdsEquipment := r.heldEquipment[slot]
		if !subscribed && !holdsAppearance && !holdsEquipment {
			continue
		}
		delete(r.subscribed, slot)
		delete(r.heldAppearance, slot)
		delete(r.heldEquipment, slot)
		r.outbox.Leave(slot, leave)
	}
	delete(h.members, slot)
	if m.outbox != nil {
		h.recipients = remove(h.recipients, m)
	}
	m.changed = false
	h.names.Forget(slot)
	h.opts.OnChange()
}

// Handle applies one validated client message. It returns why the message
// was rejected, for the sender's log.
func (h *Hub) Handle(slot uint32, message *pb.ClientMessage) error {
	m, ok := h.members[slot]
	if !ok {
		return ErrUnknownPlayer
	}
	switch body := message.Message.(type) {
	case *pb.ClientMessage_Enter:
		if m.Enter() {
			h.record(slot, message)
			h.opts.OnChange()
		}
	case *pb.ClientMessage_Exit:
		h.exit(m, message)
	case *pb.ClientMessage_Context:
		return h.setContext(m, body.Context, message)
	case *pb.ClientMessage_Checkpoint:
		return h.setCheckpoint(m, body.Checkpoint, message)
	case *pb.ClientMessage_Action:
		return h.relayAction(m, body.Action, message)
	case *pb.ClientMessage_Appearance:
		return h.setAppearance(m, body.Appearance, message)
	case *pb.ClientMessage_Equipment:
		return h.setEquipment(m, body.Equipment, message)
	case *pb.ClientMessage_Chat:
		return h.chat(m, body.Chat)
	default:
		return validate.ErrUnexpected
	}
	return nil
}

// Tick hands every checkpoint that changed since the last tick to its
// subscribers and wakes their writers.
func (h *Hub) Tick() {
	for _, m := range h.changed {
		if m.changed {
			h.distribute(m)
		}
	}
	clear(h.changed)
	h.changed = h.changed[:0]
	for _, r := range h.touched {
		r.touched = false
		r.outbox.Signal()
	}
	clear(h.touched)
	h.touched = h.touched[:0]
}

// Player returns a player's state for reading on the hub goroutine.
func (h *Hub) Player(slot uint32) *world.Player {
	if m, ok := h.members[slot]; ok {
		return m.Player
	}
	return nil
}

// Players calls f for every player.
func (h *Hub) Players(f func(p *world.Player, subscriptions int)) {
	for _, m := range h.members {
		f(m.Player, len(m.subscribed))
	}
}

// Name returns the player's name and index, as chat shows them.
func (h *Hub) Name(slot uint32) (string, uint32) {
	m, ok := h.members[slot]
	if !ok || m.Appearance == nil {
		return "", 1
	}
	return m.Appearance.GetName(), m.NameIndex
}

// InitialMessages returns the player's current state as client messages in
// the order a peer needs them, so a capture that starts mid-session replays
// on its own.
func (h *Hub) InitialMessages(slot uint32) []*pb.ClientMessage {
	m, ok := h.members[slot]
	if !ok {
		return nil
	}
	var messages []*pb.ClientMessage
	if m.InWorld {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	} else {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}})
	}
	if m.Appearance != nil {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: m.Appearance}})
	}
	if m.Equipment != nil {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Equipment{Equipment: m.Equipment}})
	}
	if m.Context != nil {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: m.Context}})
	}
	if m.Checkpoint != nil {
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: m.Checkpoint}})
	}
	return messages
}

func (h *Hub) exit(m *member, message *pb.ClientMessage) {
	if !m.Exit() {
		return
	}
	m.changed = false
	h.record(m.Slot, message)
	h.unsubscribeAll(m, pb.LeaveReason_LEAVE_REASON_EXITED)
	h.opts.OnChange()
}

func (h *Hub) setContext(m *member, c *pb.Context, message *pb.ClientMessage) error {
	changed, err := m.SetContext(c)
	if err != nil || !changed {
		return err
	}
	h.record(m.Slot, message)
	h.opts.OnChange()
	if !m.Placed() || len(m.subscribers) == 0 {
		return nil
	}
	h.distribute(m)
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerContext{
		PeerContext: &pb.PeerContext{Slot: m.Slot, Context: c}}})
	for _, r := range m.subscribers {
		r.outbox.Append(m.Slot, bytes)
	}
	return nil
}

func (h *Hub) setCheckpoint(m *member, c *pb.Checkpoint, message *pb.ClientMessage) error {
	wasPlaced := m.Placed()
	if err := m.SetCheckpoint(c); err != nil {
		return err
	}
	h.record(m.Slot, message)
	h.opts.OnChange()
	if !wasPlaced {
		for _, r := range h.recipients {
			if h.visible(r, m) {
				h.subscribe(r, m)
			}
		}
		return nil
	}
	if !m.changed {
		m.changed = true
		h.changed = append(h.changed, m)
	}
	return nil
}

func (h *Hub) relayAction(m *member, a *pb.Action, message *pb.ClientMessage) error {
	if !m.actions.Allow(h.opts.Clock.Now(), h.opts.ActionsPerSecond, h.opts.ActionBurst) {
		return ErrRateLimited
	}
	if err := m.AcceptAction(a); err != nil {
		return err
	}
	h.record(m.Slot, message)
	if len(m.subscribers) == 0 {
		return nil
	}
	h.distribute(m)
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerAction{
		PeerAction: &pb.PeerAction{Slot: m.Slot, Action: a}}})
	anchor, sequence := a.GetCheckpointSequence(), m.Checkpoint.GetSequence()
	for _, r := range m.subscribers {
		r.outbox.AppendAnchored(m.Slot, anchor, sequence, m.Entry, bytes)
	}
	return nil
}

func (h *Hub) setAppearance(m *member, a *pb.Appearance, message *pb.ClientMessage) error {
	if err := m.SetAppearance(a, h.names); err != nil {
		return err
	}
	h.record(m.Slot, message)
	h.opts.OnChange()
	if len(m.subscribers) == 0 {
		return nil
	}
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerAppearance{
		PeerAppearance: &pb.PeerAppearance{Slot: m.Slot, NameIndex: m.NameIndex, Appearance: a}}})
	for _, r := range m.subscribers {
		r.heldAppearance[m.Slot] = a.GetRevision()
		r.outbox.Replace(m.Slot, delivery.ReplaceAppearance, bytes)
	}
	return nil
}

func (h *Hub) setEquipment(m *member, e *pb.Equipment, message *pb.ClientMessage) error {
	if err := m.SetEquipment(e); err != nil {
		return err
	}
	h.record(m.Slot, message)
	if len(m.subscribers) == 0 {
		return nil
	}
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerEquipment{
		PeerEquipment: &pb.PeerEquipment{Slot: m.Slot, Equipment: e}}})
	for _, r := range m.subscribers {
		r.heldEquipment[m.Slot] = e.GetRevision()
		r.outbox.Replace(m.Slot, delivery.ReplaceEquipment, bytes)
	}
	return nil
}

// chat relays a line to every connected client, the sender included, so
// everyone sees one order and the sender's own line confirms delivery. Chat
// is global and never recorded.
func (h *Hub) chat(m *member, request *pb.ChatSend) error {
	text, err := validate.ChatText(request.GetText())
	if err == nil && !m.chat.allow(h.opts.Clock.Now()) {
		err = ErrRateLimited
	}
	if m.outbox != nil {
		result := &pb.Result{RequestId: request.GetRequestId(), Ok: err == nil}
		if err != nil {
			result.Error = err.Error()
		}
		m.outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Result{Result: result}}))
	}
	if err != nil {
		return err
	}
	name, index := h.Name(m.Slot)
	line := &pb.ChatMessage{Slot: m.Slot, Name: name, NameIndex: index, Text: text}
	if m.Appearance != nil {
		line.Level = m.Appearance.GetLevel()
	}
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_ChatMessage{ChatMessage: line}})
	for _, r := range h.recipients {
		r.outbox.AppendGlobal(bytes)
	}
	return nil
}

// visible is the interest rule: a connected client receives every placed
// peer but itself.
func (h *Hub) visible(r, peer *member) bool {
	return r.outbox != nil && r != peer && peer.Placed()
}

func (h *Hub) subscribe(r, peer *member) {
	if _, ok := r.subscribed[peer.Slot]; ok {
		return
	}
	r.subscribed[peer.Slot] = struct{}{}
	peer.subscribers = append(peer.subscribers, r)
	enter := &pb.PeerEnter{Slot: peer.Slot, Context: peer.Context, State: peer.Checkpoint}
	if a := peer.Appearance; a != nil && a.GetRevision() > r.heldAppearance[peer.Slot] {
		enter.Appearance, enter.NameIndex = a, peer.NameIndex
		r.heldAppearance[peer.Slot] = a.GetRevision()
	}
	if e := peer.Equipment; e != nil && e.GetRevision() > r.heldEquipment[peer.Slot] {
		enter.Equipment = e
		r.heldEquipment[peer.Slot] = e.GetRevision()
	}
	r.outbox.Enter(peer.Slot, peer.Checkpoint.GetSequence(),
		wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerEnter{PeerEnter: enter}}))
}

func (h *Hub) unsubscribeAll(peer *member, reason pb.LeaveReason) {
	leave := leaveMessage(peer.Slot, reason)
	for _, r := range peer.subscribers {
		delete(r.subscribed, peer.Slot)
		r.outbox.Leave(peer.Slot, leave)
	}
	clear(peer.subscribers)
	peer.subscribers = peer.subscribers[:0]
}

// distribute hands the player's latest checkpoint to its subscribers now, so
// an ordered message about it that follows is not overtaken.
func (h *Hub) distribute(m *member) {
	if !m.changed {
		return
	}
	m.changed = false
	sequence := m.Checkpoint.GetSequence()
	for _, r := range m.subscribers {
		r.outbox.SetCheckpoint(m.Slot, sequence, m.Entry)
		if !r.touched {
			r.touched = true
			h.touched = append(h.touched, r)
		}
	}
}

func (h *Hub) record(slot uint32, message *pb.ClientMessage) {
	if h.opts.Recorder != nil {
		h.opts.Recorder.Record(slot, message)
	}
}

func leaveMessage(slot uint32, reason pb.LeaveReason) []byte {
	return wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerLeave{
		PeerLeave: &pb.PeerLeave{Slot: slot, Reason: reason}}})
}

func remove(members []*member, m *member) []*member {
	if i := slices.Index(members, m); i >= 0 {
		last := len(members) - 1
		members[i] = members[last]
		members[last] = nil
		return members[:last]
	}
	return members
}
