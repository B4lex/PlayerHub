package hub

import (
	"errors"
	"log/slog"
	"math"
	"slices"
	"time"

	"playerhub/server/internal/clock"
	"playerhub/server/internal/delivery"
	"playerhub/server/internal/interest"
	"playerhub/server/internal/limit"
	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
	"playerhub/server/internal/world"
)

var (
	ErrUnknownPlayer    = errors.New("unknown player")
	ErrRateLimited      = errors.New("rate limited")
	ErrNotPlaced        = errors.New("not in world")
	ErrUnknownRecipient = errors.New("unknown recipient")
	ErrTooManyVanished  = errors.New("too many vanished players")
)

type Recorder interface {
	Record(slot uint32, m *pb.ClientMessage)
	Stop(slot uint32, reason string)
}

type Options struct {
	Clock              clock.Clock
	ActionsPerSecond   float64
	ActionBurst        float64
	Interest           interest.Config
	EntersPerSecond    float64
	ChatNearbyRadius   float64
	TradeRequestRadius float64
	TradeKeepRadius    float64
	TradeInviteSeconds float64
	Recorder           Recorder
	Logger             *slog.Logger
	OnChange           func()
}

type member struct {
	*world.Player
	outbox *delivery.Outbox
	view   interest.View

	subscribers    []*member
	subscribed     map[uint32]*subscription
	heldAppearance map[uint32]uint64
	heldEquipment  map[uint32]uint64
	nearby         map[uint32]uint64
	nearbyDue      time.Time

	changed     bool
	distributed *pb.Checkpoint
	forceNext   bool
	touched     bool

	actions limit.TokenBucket
	chat    chatThrottle
	enters  limit.TokenBucket
	inspect limit.TokenBucket

	trade         *trade
	tradeRequests limit.TokenBucket
	tradeOffers   limit.TokenBucket
}

type subscription struct {
	since    time.Time
	interval time.Duration
	weight   float64
}

type candidate struct {
	peer      *member
	distance  float64
	incumbent bool
	pinned    bool
}

type Identity struct {
	Session     uint64
	DisplayName string
	Token       []byte
}

type Hub struct {
	opts       Options
	members    map[uint32]*member
	recipients []*member
	nextSlot   uint32
	sessions   map[uint64]uint32
	changed    []*member
	touched    []*member

	rules      *interest.Rules
	grid       *interest.Grid
	hidden     interest.Hidden
	pairs      interest.Pairs
	candidates []candidate

	nearbyGrid   *interest.Grid
	nearbyPass   uint64
	nearbyJoined []uint32
	nearbyLeft   []uint32

	trades    map[uint32]*trade
	nextTrade uint32
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
	if opts.ChatNearbyRadius == 0 {
		opts.ChatNearbyRadius = 8192
	}
	if opts.TradeRequestRadius == 0 {
		opts.TradeRequestRadius, opts.TradeKeepRadius = 400, 1000
	}
	if opts.TradeInviteSeconds == 0 {
		opts.TradeInviteSeconds = 30
	}
	h := &Hub{
		opts: opts, members: make(map[uint32]*member), sessions: make(map[uint64]uint32),
		grid: interest.NewGrid(opts.Interest.LeaveRadius), hidden: interest.Hidden{}, pairs: interest.Pairs{},
		nearbyGrid: interest.NewGrid(opts.ChatNearbyRadius), trades: make(map[uint32]*trade),
	}
	h.rules = interest.NewRules(opts.Interest, h.hidden, h.pairs, interest.Vanished{})
	return h
}

func (h *Hub) Connect(kind world.Kind, outbox *delivery.Outbox) uint32 {
	return h.connect(kind, outbox, Identity{})
}

func (h *Hub) ConnectClient(outbox *delivery.Outbox, identity Identity) (slot, replaced uint32) {
	if previous, ok := h.sessions[identity.Session]; ok {
		replaced = previous
		h.Disconnect(previous)
	}
	slot = h.connect(world.KindClient, outbox, identity)
	h.sessions[identity.Session] = slot
	m := h.members[slot]
	roster := &pb.Roster{}
	for _, r := range h.recipients {
		if r.listed() {
			roster.Joined = append(roster.Joined, &pb.RosterEntry{Slot: r.Slot, Name: r.DisplayName, SessionId: r.Session})
		}
	}
	outbox.AppendGlobal(rosterMessage(roster))
	h.announce(m, &pb.Roster{Joined: []*pb.RosterEntry{{Slot: slot, Name: identity.DisplayName,
		SessionId: identity.Session}}})
	return slot, replaced
}

func (m *member) listed() bool {
	return m.outbox != nil && m.Session != 0
}

func rosterMessage(roster *pb.Roster) []byte {
	return wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Roster{Roster: roster}})
}

func (h *Hub) announce(m *member, roster *pb.Roster) {
	bytes := rosterMessage(roster)
	for _, r := range h.recipients {
		if r != m && r.listed() {
			r.outbox.AppendGlobal(bytes)
		}
	}
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
		nearby:         make(map[uint32]uint64),
	}
	m.view.Slot, m.view.Session = m.Slot, identity.Session
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

func (h *Hub) Disconnect(slot uint32) {
	m, ok := h.members[slot]
	if !ok {
		return
	}
	if h.opts.Recorder != nil {
		h.opts.Recorder.Stop(slot, "disconnected")
	}
	if m.trade != nil {
		h.closeTrade(m.trade, pb.TradeCloseReason_TRADE_CLOSE_REASON_DISCONNECTED)
	}
	for peer := range m.subscribed {
		h.members[peer].subscribers = remove(h.members[peer].subscribers, m)
	}
	leave := leaveMessage(slot, pb.LeaveReason_LEAVE_REASON_DISCONNECTED)
	for _, r := range h.recipients {
		// The roster's leave takes the slot out of the client's nearby players too.
		delete(r.nearby, slot)
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
	h.nearbyGrid.Remove(slot)
	delete(h.hidden, slot)
	for pair := range h.pairs {
		if pair.Viewer == slot || pair.Target == slot {
			delete(h.pairs, pair)
		}
	}
	m.changed = false
	if h.sessions[m.Session] == slot {
		delete(h.sessions, m.Session)
	}
	if m.listed() {
		h.announce(m, &pb.Roster{Left: slot})
	}
	h.opts.OnChange()
}

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
	case *pb.ClientMessage_Vanish:
		return h.vanish(m, body.Vanish)
	case *pb.ClientMessage_Inspect:
		return h.inspect(m, body.Inspect)
	case *pb.ClientMessage_TradeRequest:
		return h.requestTrade(m, body.TradeRequest)
	case *pb.ClientMessage_TradeAnswer:
		return h.answerTrade(m, body.TradeAnswer)
	case *pb.ClientMessage_TradeOffer:
		return h.offerTrade(m, body.TradeOffer)
	case *pb.ClientMessage_TradeReport:
		return h.reportTrade(m, body.TradeReport)
	case *pb.ClientMessage_TradeAccept:
		return h.acceptTrade(m, body.TradeAccept)
	case *pb.ClientMessage_TradeCancel:
		return h.cancelTrade(m, body.TradeCancel)
	default:
		return validate.ErrUnexpected
	}
	return nil
}

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

func (h *Hub) UpdateInterest() {
	now := h.opts.Clock.Now()
	for _, r := range h.recipients {
		if r.Placed() {
			h.evaluateViewer(r, now)
		}
	}
	h.updateNearbyPlayers(now)
	h.expireTrades(now)
}

func (h *Hub) SetViewRadius(slot uint32, radius float64) {
	if m, ok := h.members[slot]; ok {
		m.view.Radius = radius
	}
}

func (h *Hub) SetVanished(slot uint32, sessions []uint64) {
	m, ok := h.members[slot]
	if !ok || len(sessions) == 0 {
		return
	}
	m.view.Vanished = make(map[uint64]bool, len(sessions))
	for _, session := range sessions {
		m.view.Vanished[session] = true
	}
}

func (h *Hub) vanish(m *member, v *pb.Vanish) error {
	session, vanished := v.GetSessionId(), v.GetVanished()
	if m.view.Vanished[session] == vanished {
		return nil
	}
	if vanished {
		if len(m.view.Vanished) >= validate.MaximumVanished {
			return ErrTooManyVanished
		}
		if m.view.Vanished == nil {
			m.view.Vanished = make(map[uint64]bool)
		}
		m.view.Vanished[session] = true
	} else {
		delete(m.view.Vanished, session)
	}
	if slot, ok := h.sessions[session]; ok {
		h.evaluatePair(m, h.members[slot], h.opts.Clock.Now())
	}
	return nil
}

func (h *Hub) inspect(m *member, request *pb.Inspect) error {
	if m.outbox == nil {
		return nil
	}
	if !m.inspect.Allow(h.opts.Clock.Now(), inspectsPerSecond, inspectBurst) {
		return ErrRateLimited
	}
	inspection := &pb.Inspection{Slot: request.GetSlot()}
	if target := h.members[request.GetSlot()]; target != nil && target.listed() && target.Appearance != nil {
		a := target.Appearance
		inspection.Level, inspection.Race, inspection.DefaultRace = a.GetLevel(), a.GetRace(), a.GetDefaultRace()
	}
	m.outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Inspection{Inspection: inspection}}))
	return nil
}

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

func (h *Hub) Hidden(slot uint32) bool {
	return h.hidden[slot]
}

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

func (h *Hub) Player(slot uint32) *world.Player {
	if m, ok := h.members[slot]; ok {
		return m.Player
	}
	return nil
}

func (h *Hub) Players(f func(p *world.Player, visible int, outbox *delivery.Outbox)) {
	for _, m := range h.members {
		f(m.Player, len(m.subscribed), m.outbox)
	}
}

func (h *Hub) Name(slot uint32) string {
	if m, ok := h.members[slot]; ok {
		return m.Name()
	}
	return ""
}

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
	h.checkTradeRange(m)
	h.opts.OnChange()
}

func (h *Hub) unplace(m *member, reason pb.LeaveReason) {
	h.grid.Remove(m.Slot)
	h.nearbyGrid.Remove(m.Slot)
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
		// The stored position belonged to the old space; the next checkpoint places the player
		// in the new one.
		m.changed = false
		if wasPlaced {
			h.unplace(m, pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE)
		}
		h.checkTradeRange(m)
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
	m.view.Space, m.view.X, m.view.Y = interest.SpaceOf(m.Context), float64(c.X), float64(c.Y)
	h.grid.Update(&m.view)
	nearby := nearbyView(&m.view)
	h.nearbyGrid.Update(&nearby)
	h.checkTradeRange(m)
	if !wasPlaced {
		// PeerEnter carries this checkpoint.
		m.distributed, m.forceNext = c, false
		now := h.opts.Clock.Now()
		h.evaluateTarget(m, now)
		h.evaluateViewer(m, now)
		return nil
	}
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

func (h *Hub) speaker(m *member) *pb.ChatSpeaker {
	return &pb.ChatSpeaker{Slot: m.Slot, Name: m.Name(), Level: m.Appearance.GetLevel(), SessionId: m.Session}
}

func (h *Hub) chat(m *member, request *pb.ChatSend) error {
	line := &pb.ChatMessage{Channel: request.GetChannel()}
	text, err := validate.Chat(request)
	var recipient *member
	if err == nil {
		switch line.Channel {
		case pb.ChatChannel_CHAT_CHANNEL_NEARBY:
			if !m.Placed() {
				err = ErrNotPlaced
			}
		case pb.ChatChannel_CHAT_CHANNEL_WHISPER:
			recipient = h.members[request.GetRecipient()]
			if recipient == nil || recipient.outbox == nil {
				err = ErrUnknownRecipient
			}
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
	case pb.ChatChannel_CHAT_CHANNEL_NEARBY:
		for _, r := range h.recipients {
			if h.hearsNearby(r, m) {
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

func (h *Hub) hearsNearby(r, sender *member) bool {
	if !r.Placed() || r.view.Space != sender.view.Space {
		return false
	}
	dx, dy := r.view.X-sender.view.X, r.view.Y-sender.view.Y
	return r.view.Space.Worldspace == 0 || dx*dx+dy*dy <= h.opts.ChatNearbyRadius*h.opts.ChatNearbyRadius
}

// nearbyView places a whole interior in one cell of nearbyGrid, since Nearby chat ignores distance there.
func nearbyView(v *interest.View) interest.View {
	if v.Space.Worldspace == 0 {
		return interest.View{Slot: v.Slot, Space: v.Space}
	}
	return interest.View{Slot: v.Slot, Space: v.Space, X: v.X, Y: v.Y}
}

// A scan checks every player in reach, so in a crowd each viewer is rescanned once a second, the
// viewers spread over the interest passes.
const nearbyPlayersInterval = time.Second

func (h *Hub) updateNearbyPlayers(now time.Time) {
	for _, r := range h.recipients {
		if r.listed() && !now.Before(r.nearbyDue) {
			r.nearbyDue = now.Add(nearbyPlayersInterval)
			h.updateNearby(r)
		}
	}
}

func (h *Hub) updateNearby(r *member) {
	joined, left := h.nearbyJoined[:0], h.nearbyLeft[:0]
	kept := 0
	h.eachNearby(r, func(slot uint32) {
		if _, known := r.nearby[slot]; known {
			kept++
		} else {
			joined = append(joined, slot)
		}
	})
	if kept < len(r.nearby) {
		h.nearbyPass++
		h.eachNearby(r, func(slot uint32) { r.nearby[slot] = h.nearbyPass })
		for slot, pass := range r.nearby {
			if pass != h.nearbyPass {
				left = append(left, slot)
				delete(r.nearby, slot)
			}
		}
	}
	for _, slot := range joined {
		r.nearby[slot] = h.nearbyPass
	}
	if len(joined) > 0 || len(left) > 0 {
		r.outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_NearbyPlayers{
			NearbyPlayers: &pb.NearbyPlayers{Joined: joined, Left: left}}}))
	}
	h.nearbyJoined, h.nearbyLeft = joined, left
}

func (h *Hub) eachNearby(r *member, f func(slot uint32)) {
	if !r.Placed() {
		return
	}
	view := nearbyView(&r.view)
	h.nearbyGrid.Near(&view, func(slot uint32) {
		if peer := h.members[slot]; peer != r && peer.listed() && h.hearsNearby(r, peer) {
			f(slot)
		}
	})
}

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

func (h *Hub) allowEnter(r *member, now time.Time) bool {
	rate := h.opts.EntersPerSecond
	return rate <= 0 || r.enters.Allow(now, rate, max(rate, 1))
}

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

const fullPriorityDistance = 1024

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
