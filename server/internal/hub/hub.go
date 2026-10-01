// Package hub owns all world state. One goroutine calls its methods: sessions,
// playback, and the admin API submit work to it through Loop, and tests call
// the methods directly with a fake clock.
package hub

import (
	"errors"
	"log/slog"
	"math"
	"slices"
	"time"

	"skyrim-mp/server/internal/clock"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/interest"
	"skyrim-mp/server/internal/limit"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
	"skyrim-mp/server/internal/world"
)

var (
	ErrUnknownPlayer = errors.New("unknown player")
	ErrRateLimited   = errors.New("rate limited")
	// Local chat needs the sender's position.
	ErrNotPlaced        = errors.New("not in world")
	ErrUnknownRecipient = errors.New("unknown recipient")
	ErrEmptyWhisper     = errors.New("empty whisper")
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
	// Zero means interest.DefaultConfig.
	Interest interest.Config
	// New subscriptions per viewer and second, nearest first; zero is no limit.
	EntersPerSecond float64
	// How far local chat reaches in a worldspace; zero is 8192.
	ChatLocalRadius float64
	Recorder        Recorder
	Logger          *slog.Logger
	// Called whenever something the admin panel shows changes.
	OnChange func()
}

type member struct {
	*world.Player
	// Nil for a virtual player, which receives nothing.
	outbox *delivery.Outbox
	// Where the player is, while placed.
	view interest.View
	// The enter radius the client asked for, zero for the server's.
	viewRadius float64

	// Recipients subscribed to this player.
	subscribers []*member
	// Peers this player receives, and the presentation revisions it holds.
	subscribed     map[uint32]*subscription
	heldAppearance map[uint32]uint64
	heldEquipment  map[uint32]uint64

	// A checkpoint arrived since the last tick.
	changed bool
	// The last checkpoint handed to subscribers, to skip identical ones.
	distributed *pb.Checkpoint
	// The next checkpoint is distributed even if identical: it follows a
	// context change, which clients only apply with a checkpoint.
	forceNext bool
	// The outbox received checkpoints this tick.
	touched bool

	actions limit.TokenBucket
	chat    chatThrottle
	// New subscriptions of this viewer, when limited.
	enters limit.TokenBucket
}

type subscription struct {
	since time.Time
	// The least time between written checkpoints and the budget weight: the
	// pair's rate class.
	interval time.Duration
	weight   float64
}

// candidate is a peer a viewer may receive in this interest pass.
type candidate struct {
	peer      *member
	distance  float64
	incumbent bool
	// Still within its minimum time in view, so the cap cannot evict it.
	pinned bool
}

// Identity is the session a client connected with.
type Identity struct {
	Session     uint64
	DisplayName string
	// The session's secret when it was issued for this connection. The
	// welcome carries it once.
	Token []byte
}

type Hub struct {
	opts       Options
	members    map[uint32]*member
	recipients []*member
	nextSlot   uint32
	names      *world.Names
	// The connected client of each session.
	sessions map[uint64]uint32
	changed  []*member
	touched  []*member

	rules  *interest.Rules
	grid   *interest.Grid
	hidden interest.Hidden
	pairs  interest.Pairs
	// Reused by every interest pass.
	candidates []candidate
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
	if opts.Interest == (interest.Config{}) {
		opts.Interest = interest.DefaultConfig()
	}
	if opts.ChatLocalRadius == 0 {
		opts.ChatLocalRadius = 8192
	}
	h := &Hub{
		opts: opts, members: make(map[uint32]*member), names: world.NewNames(), sessions: make(map[uint64]uint32),
		grid: interest.NewGrid(opts.Interest.LeaveRadius), hidden: interest.Hidden{}, pairs: interest.Pairs{},
	}
	h.rules = interest.NewRules(opts.Interest, h.hidden, h.pairs)
	return h
}

// Connect adds a player without a session and returns its slot. A player
// with an outbox is welcomed; it receives peers once it is placed itself.
func (h *Hub) Connect(kind world.Kind, outbox *delivery.Outbox) uint32 {
	return h.connect(kind, outbox, Identity{})
}

// ConnectClient adds a client under its session. A session is connected
// once, so the client it was connected as before is disconnected first, and
// its slot is returned as replaced, zero for none, for the caller to close.
func (h *Hub) ConnectClient(outbox *delivery.Outbox, identity Identity) (slot, replaced uint32) {
	if previous, ok := h.sessions[identity.Session]; ok {
		replaced = previous
		h.Disconnect(previous)
	}
	slot = h.connect(world.KindClient, outbox, identity)
	h.sessions[identity.Session] = slot
	h.names.Set(slot, identity.DisplayName)
	return slot, replaced
}

func (h *Hub) connect(kind world.Kind, outbox *delivery.Outbox, identity Identity) uint32 {
	h.nextSlot++
	m := &member{
		Player: &world.Player{Slot: h.nextSlot, Kind: kind, ConnectedAt: h.opts.Clock.Now(),
			Session: identity.Session, DisplayName: identity.DisplayName},
		outbox:         outbox,
		subscribed:     make(map[uint32]*subscription),
		heldAppearance: make(map[uint32]uint64),
		heldEquipment:  make(map[uint32]uint64),
	}
	m.view.Slot = m.Slot
	h.members[m.Slot] = m
	if outbox != nil {
		h.recipients = append(h.recipients, m)
		outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Welcome{
			Welcome: &pb.Welcome{Slot: m.Slot, SessionId: identity.Session, DisplayName: identity.DisplayName,
				SessionToken: identity.Token}}}))
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
	h.grid.Remove(slot)
	delete(h.hidden, slot)
	for pair := range h.pairs {
		if pair.Viewer == slot || pair.Target == slot {
			delete(h.pairs, pair)
		}
	}
	m.changed = false
	h.names.Forget(slot)
	if h.sessions[m.Session] == slot {
		delete(h.sessions, m.Session)
	}
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

// UpdateInterest reevaluates every viewer's pairs: the periodic pass that
// applies distance and time in view. Hard rules take effect at once without
// it.
func (h *Hub) UpdateInterest() {
	now := h.opts.Clock.Now()
	for _, r := range h.recipients {
		if r.Placed() {
			h.evaluateViewer(r, now)
		}
	}
}

// SetViewRadius sets the enter radius a client asked for; the rules clamp it.
func (h *Hub) SetViewRadius(slot uint32, radius float64) {
	if m, ok := h.members[slot]; ok {
		m.viewRadius = radius
		m.view.Radius = radius
	}
}

// SetHidden hides a player from everyone, or shows it again, at once.
func (h *Hub) SetHidden(slot uint32, hidden bool) bool {
	m, ok := h.members[slot]
	if !ok {
		return false
	}
	if hidden {
		h.hidden[slot] = true
	} else {
		delete(h.hidden, slot)
	}
	if m.Placed() {
		h.evaluateTarget(m, h.opts.Clock.Now())
	}
	h.opts.OnChange()
	return true
}

// SetExcluded makes viewer never see target, or lifts that, at once.
func (h *Hub) SetExcluded(viewer, target uint32, excluded bool) bool {
	r, ok := h.members[viewer]
	peer, found := h.members[target]
	if !ok || !found || viewer == target {
		return false
	}
	pair := interest.Pair{Viewer: viewer, Target: target}
	if excluded {
		h.pairs[pair] = true
	} else {
		delete(h.pairs, pair)
	}
	h.evaluatePair(r, peer, h.opts.Clock.Now())
	h.opts.OnChange()
	return true
}

// Hidden reports whether an admin hid the player.
func (h *Hub) Hidden(slot uint32) bool {
	return h.hidden[slot]
}

// Exclusions lists the excluded pairs, ordered by viewer then target.
func (h *Hub) Exclusions() []interest.Pair {
	pairs := make([]interest.Pair, 0, len(h.pairs))
	for pair := range h.pairs {
		pairs = append(pairs, pair)
	}
	slices.SortFunc(pairs, func(a, b interest.Pair) int {
		if a.Viewer != b.Viewer {
			return int(a.Viewer) - int(b.Viewer)
		}
		return int(a.Target) - int(b.Target)
	})
	return pairs
}

// Player returns a player's state for reading on the hub goroutine.
func (h *Hub) Player(slot uint32) *world.Player {
	if m, ok := h.members[slot]; ok {
		return m.Player
	}
	return nil
}

// Players calls f for every player with the number of peers it receives and
// its outbox, nil for a virtual player.
func (h *Hub) Players(f func(p *world.Player, visible int, outbox *delivery.Outbox)) {
	for _, m := range h.members {
		f(m.Player, len(m.subscribed), m.outbox)
	}
}

// Name returns the player's name, empty for an unknown player.
func (h *Hub) Name(slot uint32) string {
	if m, ok := h.members[slot]; ok {
		return m.Name()
	}
	return ""
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
	h.unplace(m, pb.LeaveReason_LEAVE_REASON_EXITED)
	h.opts.OnChange()
}

// unplace ends every subscription to and from the player, which has no
// position any more. Subscribers learn why with reason.
func (h *Hub) unplace(m *member, reason pb.LeaveReason) {
	h.grid.Remove(m.Slot)
	h.unsubscribeAll(m, reason)
	for peer := range m.subscribed {
		h.unsubscribe(m, h.members[peer], pb.LeaveReason_LEAVE_REASON_VIEWER_UNPLACED)
	}
}

func (h *Hub) setContext(m *member, c *pb.Context, message *pb.ClientMessage) error {
	wasPlaced := m.Placed()
	change, err := m.SetContext(c)
	if err != nil || change == world.ContextUnchanged {
		return err
	}
	h.record(m.Slot, message)
	h.opts.OnChange()
	m.forceNext = true
	if change == world.ContextMoved {
		// The stored position belonged to the old space; the next checkpoint
		// places the player in the new one.
		m.changed = false
		if wasPlaced {
			h.unplace(m, pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE)
		}
		return nil
	}
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
	m.view = interest.View{Slot: m.Slot, Space: interest.SpaceOf(m.Context), X: float64(c.X), Y: float64(c.Y),
		Radius: m.viewRadius}
	h.grid.Update(&m.view)
	if !wasPlaced {
		// PeerEnter carries this checkpoint.
		m.distributed, m.forceNext = c, false
		now := h.opts.Clock.Now()
		h.evaluateTarget(m, now)
		h.evaluateViewer(m, now)
		return nil
	}
	// An idle player repeats the same sample; subscribers already hold it. An
	// action still sends its anchor (AppendAnchored).
	if !m.forceNext && sameSample(m.distributed, c) {
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
	if err := m.SetAppearance(a); err != nil {
		return err
	}
	h.record(m.Slot, message)
	h.opts.OnChange()
	h.relayAppearance(m)
	return nil
}

// relayAppearance hands the player's stored appearance to its subscribers.
func (h *Hub) relayAppearance(m *member) {
	if len(m.subscribers) == 0 {
		return
	}
	a := m.Appearance
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerAppearance{
		PeerAppearance: &pb.PeerAppearance{Slot: m.Slot, Appearance: a}}})
	for _, r := range m.subscribers {
		r.heldAppearance[m.Slot] = a.GetRevision()
		r.outbox.Replace(m.Slot, delivery.ReplaceAppearance, bytes)
	}
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

// speaker returns the player as chat shows it.
func (h *Hub) speaker(m *member) *pb.ChatSpeaker {
	return &pb.ChatSpeaker{Slot: m.Slot, Name: m.Name(), Level: m.Appearance.GetLevel()}
}

// chat relays a line to its channel's recipients, the sender included, so
// everyone sees one order and the sender's own line confirms delivery. Chat
// is never recorded.
func (h *Hub) chat(m *member, request *pb.ChatSend) error {
	line := &pb.ChatMessage{Channel: request.GetChannel()}
	text, err := validate.Chat(request)
	var recipient *member
	if err == nil {
		switch line.Channel {
		case pb.ChatChannel_CHAT_CHANNEL_LOCAL:
			if !m.Placed() {
				err = ErrNotPlaced
			}
		case pb.ChatChannel_CHAT_CHANNEL_WHISPER:
			recipient, text, err = h.addressee(text)
		}
	}
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
	line.Sender, line.Text = h.speaker(m), text
	if recipient != nil {
		line.Recipient = h.speaker(recipient)
	}
	bytes := wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_ChatMessage{ChatMessage: line}})
	switch line.Channel {
	case pb.ChatChannel_CHAT_CHANNEL_LOCAL:
		// Chat is rate limited, so a scan costs less than keeping an index of
		// interiors, which the grid does not bound.
		for _, r := range h.recipients {
			if h.hearsLocally(r, m) {
				r.outbox.AppendGlobal(bytes)
			}
		}
	case pb.ChatChannel_CHAT_CHANNEL_WHISPER:
		if m.outbox != nil {
			m.outbox.AppendGlobal(bytes)
		}
		if recipient != m {
			recipient.outbox.AppendGlobal(bytes)
		}
	default:
		for _, r := range h.recipients {
			r.outbox.AppendGlobal(bytes)
		}
	}
	return nil
}

// addressee resolves the connected player a whisper names and returns the
// message after the name.
func (h *Hub) addressee(text string) (*member, string, error) {
	slot, message, found := h.names.Find(text, validate.MaximumNameBytes)
	if !found {
		return nil, "", ErrUnknownRecipient
	}
	recipient := h.members[slot]
	if recipient == nil || recipient.outbox == nil {
		return nil, "", ErrUnknownRecipient
	}
	if message == "" {
		return nil, "", ErrEmptyWhisper
	}
	return recipient, message, nil
}

// hearsLocally reports whether r receives local chat from sender: both placed
// in the same space and, in a worldspace, within the local radius. An
// interior is heard whole.
func (h *Hub) hearsLocally(r, sender *member) bool {
	if !r.Placed() || r.view.Space != sender.view.Space {
		return false
	}
	return r.view.Space.Worldspace == 0 || interest.Distance(&r.view, &sender.view) <= h.opts.ChatLocalRadius
}

// evaluateViewer reevaluates the viewer's subscriptions and the players near
// it, applies the cap, and sets each pair's rate class.
func (h *Hub) evaluateViewer(r *member, now time.Time) {
	if r.outbox == nil {
		return
	}
	dwell := time.Duration(h.opts.Interest.MinDwellSeconds * float64(time.Second))
	candidates := h.candidates[:0]
	for slot, sub := range r.subscribed {
		peer := h.members[slot]
		if !h.keep(r, peer, sub, now) {
			continue
		}
		candidates = append(candidates, candidate{peer: peer, distance: interest.Distance(&r.view, &peer.view),
			incumbent: true, pinned: now.Sub(sub.since) < dwell})
	}
	if r.Placed() {
		h.grid.Near(&r.view, func(slot uint32) {
			if _, subscribed := r.subscribed[slot]; subscribed || slot == r.Slot {
				return
			}
			peer := h.members[slot]
			if peer.Placed() && h.rules.Decide(&r.view, &peer.view, time.Time{}, now).Visible {
				candidates = append(candidates, candidate{peer: peer, distance: interest.Distance(&r.view, &peer.view)})
			}
		})
	}
	candidates = h.applyCap(r, candidates)
	if h.opts.EntersPerSecond > 0 {
		// Entering a crowd sends many appearances at once; the nearest go first
		// and the rest follow over the next passes.
		slices.SortStableFunc(candidates, func(a, b candidate) int {
			switch {
			case a.distance < b.distance:
				return -1
			case a.distance > b.distance:
				return 1
			}
			return 0
		})
	}
	for _, c := range candidates {
		if !c.incumbent {
			if !h.allowEnter(r, now) {
				continue
			}
			h.subscribe(r, c.peer, now)
		}
		h.setRateClass(r, c.peer, c.distance)
	}
	clear(candidates)
	h.candidates = candidates[:0]
}

// allowEnter spends one of the viewer's new subscriptions, when they are
// limited.
func (h *Hub) allowEnter(r *member, now time.Time) bool {
	rate := h.opts.EntersPerSecond
	return rate <= 0 || r.enters.Allow(now, rate, max(rate, 1))
}

// keep applies the rules to one existing subscription and ends it when they
// say so.
func (h *Hub) keep(r, peer *member, sub *subscription, now time.Time) bool {
	if !r.Placed() || !peer.Placed() {
		reason := pb.LeaveReason_LEAVE_REASON_EXITED
		if !r.Placed() {
			reason = pb.LeaveReason_LEAVE_REASON_VIEWER_UNPLACED
		}
		h.unsubscribe(r, peer, reason)
		return false
	}
	if verdict := h.rules.Decide(&r.view, &peer.view, sub.since, now); !verdict.Visible {
		h.unsubscribe(r, peer, verdict.Reason)
		return false
	}
	return true
}

// applyCap keeps at most MaxVisible candidates, nearest first. Peers still
// within their minimum time in view are never evicted, and a peer already in
// view keeps its place unless a newcomer is CapHysteresis nearer.
func (h *Hub) applyCap(r *member, candidates []candidate) []candidate {
	limit := h.opts.Interest.MaxVisible
	if limit <= 0 || len(candidates) <= limit {
		return candidates
	}
	hysteresis := h.opts.Interest.CapHysteresis
	key := func(c candidate) float64 {
		if c.incumbent {
			return c.distance - hysteresis
		}
		return c.distance
	}
	slices.SortFunc(candidates, func(a, b candidate) int {
		if a.pinned != b.pinned {
			if a.pinned {
				return -1
			}
			return 1
		}
		switch ka, kb := key(a), key(b); {
		case ka < kb:
			return -1
		case ka > kb:
			return 1
		}
		return int(a.peer.Slot) - int(b.peer.Slot)
	})
	pinned := 0
	for _, c := range candidates {
		if c.pinned {
			pinned++
		}
	}
	limit = max(limit, pinned)
	for _, c := range candidates[limit:] {
		if c.incumbent {
			h.unsubscribe(r, c.peer, pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE)
		}
	}
	return candidates[:limit]
}

// Within this distance a waiting checkpoint has full priority under the
// budget; beyond it the priority falls with distance, so the nearest peers
// stay smooth when the budget is short.
const fullPriorityDistance = 1024

// setRateClass sets the pair's rate and, for the budget, the weight of its
// waiting checkpoints, in eighths so it changes rarely.
func (h *Hub) setRateClass(r, peer *member, distance float64) {
	sub := r.subscribed[peer.Slot]
	interval := h.rules.Interval(distance)
	weight := 1.0
	if distance > fullPriorityDistance {
		weight = max(math.Round(fullPriorityDistance/distance*8), 1) / 8
	}
	if interval != sub.interval || weight != sub.weight {
		sub.interval, sub.weight = interval, weight
		r.outbox.SetClass(peer.Slot, interval, weight)
	}
}

// evaluateTarget reevaluates the target's subscribers and the viewers near
// it.
func (h *Hub) evaluateTarget(peer *member, now time.Time) {
	for _, r := range slices.Clone(peer.subscribers) {
		h.evaluatePair(r, peer, now)
	}
	if !peer.Placed() {
		return
	}
	h.grid.Near(&peer.view, func(slot uint32) {
		r := h.members[slot]
		if _, subscribed := r.subscribed[peer.Slot]; !subscribed && r.outbox != nil && r != peer {
			h.evaluatePair(r, peer, now)
		}
	})
}

// evaluatePair subscribes r to peer or ends the subscription, as the rules
// decide. Both must be placed to be compared.
func (h *Hub) evaluatePair(r, peer *member, now time.Time) {
	if r.outbox == nil || r == peer {
		return
	}
	if sub, subscribed := r.subscribed[peer.Slot]; subscribed {
		h.keep(r, peer, sub, now)
		return
	}
	// A full view is left to the periodic pass, which ranks by distance.
	if !r.Placed() || !peer.Placed() ||
		(h.opts.Interest.MaxVisible > 0 && len(r.subscribed) >= h.opts.Interest.MaxVisible) {
		return
	}
	if h.rules.Decide(&r.view, &peer.view, time.Time{}, now).Visible && h.allowEnter(r, now) {
		h.subscribe(r, peer, now)
		h.setRateClass(r, peer, interest.Distance(&r.view, &peer.view))
	}
}

func (h *Hub) subscribe(r, peer *member, now time.Time) {
	r.subscribed[peer.Slot] = &subscription{since: now, weight: 1}
	peer.subscribers = append(peer.subscribers, r)
	enter := &pb.PeerEnter{Slot: peer.Slot, Context: peer.Context, State: peer.Checkpoint}
	if a := peer.Appearance; a != nil && a.GetRevision() > r.heldAppearance[peer.Slot] {
		enter.Appearance = a
		r.heldAppearance[peer.Slot] = a.GetRevision()
	}
	if e := peer.Equipment; e != nil && e.GetRevision() > r.heldEquipment[peer.Slot] {
		enter.Equipment = e
		r.heldEquipment[peer.Slot] = e.GetRevision()
	}
	r.outbox.Enter(peer.Slot, peer.Checkpoint.GetSequence(),
		wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_PeerEnter{PeerEnter: enter}}))
}

func (h *Hub) unsubscribe(r, peer *member, reason pb.LeaveReason) {
	delete(r.subscribed, peer.Slot)
	peer.subscribers = remove(peer.subscribers, r)
	r.outbox.Leave(peer.Slot, leaveMessage(peer.Slot, reason))
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
	m.distributed, m.forceNext = m.Checkpoint, false
	sequence := m.Checkpoint.GetSequence()
	now := h.opts.Clock.Now()
	for _, r := range m.subscribers {
		r.outbox.SetCheckpoint(m.Slot, sequence, m.Entry, now)
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

// sameSample compares everything but the sequence.
func sameSample(a, b *pb.Checkpoint) bool {
	return a != nil && a.X == b.X && a.Y == b.Y && a.Z == b.Z && a.Heading == b.Heading && a.Pitch == b.Pitch &&
		a.Speed == b.Speed && a.Direction == b.Direction && a.Flags == b.Flags
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
