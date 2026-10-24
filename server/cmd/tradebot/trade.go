package main

import (
	"slices"
	"strings"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

type phase int

const (
	phaseIdle phase = iota
	phaseRequesting
	phaseInviting
	phaseInvited
	phaseOpen
)

var phaseNames = [...]string{"idle", "requesting", "inviting", "invited", "open"}

// Matches the plugin's TradeSession: a commit may name a revision an edit in flight replaced.
const keptRevisions = 16

type trade struct {
	id    uint32
	phase phase
	peer  string

	ownRevision uint64
	sent        map[uint64][]*pb.TradeItem
	// The peer's report on ownRevision; nil until it arrives.
	peerSkips []*pb.TradeSkip
	reported  bool

	peerRevision uint64
	peerItems    []*pb.TradeItem
	ownSkips     []*pb.TradeSkip
	reportedPeer uint64

	ownAccepted, peerAccepted bool
	// The bot's plugin queries the peer has not answered, oldest first.
	pluginQueries [][]string
}

func (b *bot) handle(m *pb.ServerMessage) {
	if b.handlePeer(m) {
		return
	}
	t := &b.trade
	switch body := m.Message.(type) {
	case *pb.ServerMessage_TradeInvite:
		invite := body.TradeInvite
		if invite.GetSender().GetSlot() == b.slot {
			*t = trade{id: invite.GetTradeId(), phase: phaseInviting, peer: invite.GetRecipient().GetName()}
			say("trade %d: invited %s; waiting for the answer", t.id, t.peer)
			return
		}
		*t = trade{id: invite.GetTradeId(), phase: phaseInvited, peer: invite.GetSender().GetName()}
		if b.autoAnswer {
			say("trade %d: %s invites the bot; accepting", t.id, t.peer)
			b.answer(true)
		} else {
			say("trade %d: %s invites the bot; accept or decline", t.id, t.peer)
		}
	case *pb.ServerMessage_TradeOpen:
		if body.TradeOpen.GetTradeId() != t.id {
			return
		}
		t.phase = phaseOpen
		t.sent = make(map[uint64][]*pb.TradeItem)
		say("trade %d: open with %s", t.id, t.peer)
		b.queryPlugins()
		if len(b.offer) > 0 {
			b.sendOffer()
		}
	case *pb.ServerMessage_TradeOffer:
		b.receiveOffer(body.TradeOffer)
	case *pb.ServerMessage_TradeReport:
		b.receiveReport(body.TradeReport)
	case *pb.ServerMessage_TradePluginQuery:
		b.answerPlugins(body.TradePluginQuery)
	case *pb.ServerMessage_TradePluginReply:
		b.receivePluginReply(body.TradePluginReply)
	case *pb.ServerMessage_TradeAccept:
		accept := body.TradeAccept
		if accept.GetTradeId() != t.id || t.phase != phaseOpen {
			return
		}
		if accept.GetPeerRevision() != t.peerRevision || accept.GetOwnRevision() != t.ownRevision {
			say("trade %d: %s accepted revisions %d/%d, not the current %d/%d", t.id, t.peer,
				accept.GetPeerRevision(), accept.GetOwnRevision(), t.peerRevision, t.ownRevision)
			return
		}
		t.peerAccepted = true
		say("trade %d: %s accepted its revision %d and the bot's %d", t.id, t.peer, t.peerRevision, t.ownRevision)
		if b.autoAccept && !t.ownAccepted {
			b.accept()
		}
	case *pb.ServerMessage_TradeCommit:
		b.commit(body.TradeCommit)
	case *pb.ServerMessage_TradeClose:
		c := body.TradeClose
		reason := closeReasonName(c.GetReason())
		switch {
		case c.GetTradeId() == 0:
			say("trade request refused: %s", reason)
			if t.phase == phaseRequesting {
				*t = trade{}
			}
		case c.GetTradeId() == t.id:
			say("trade %d: closed, %s", t.id, reason)
			*t = trade{}
		}
	case *pb.ServerMessage_ChatResult:
		if r := body.ChatResult; r.GetRejection() != pb.ChatRejection_CHAT_REJECTION_NONE {
			say("chat message %d rejected: %s", r.GetRequestId(),
				strings.ToLower(strings.TrimPrefix(r.GetRejection().String(), "CHAT_REJECTION_")))
		}
	}
}

func (b *bot) receiveOffer(offer *pb.TradeOffer) {
	t := &b.trade
	if offer.GetTradeId() != t.id || t.phase != phaseOpen {
		return
	}
	t.peerRevision, t.peerItems = offer.GetRevision(), offer.GetItems()
	t.ownAccepted, t.peerAccepted = false, false
	t.ownSkips = b.reportSkips(t.peerItems)
	t.reportedPeer = t.peerRevision
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeReport{TradeReport: &pb.TradeReport{
		TradeId: t.id, Revision: t.peerRevision, Skipped: t.ownSkips}}})
	say("trade %d: %s offers revision %d:", t.id, t.peer, t.peerRevision)
	printItems(t.peerItems, t.ownSkips, "    ")
	if len(t.ownSkips) > 0 {
		say("reported %d line(s) as not receivable", len(t.ownSkips))
	}
	if b.mirror {
		b.copyPeer(nil)
	}
}

// queryPlugins asks about the plugins of the bot's offer as the plugin does for its inventory.
func (b *bot) queryPlugins() {
	t := &b.trade
	var plugins []string
	for _, item := range b.offer {
		forms := append([]*pb.FormRef{item.GetItem(), item.GetEnchantment()}, effectForms(item)...)
		for _, f := range forms {
			if f != nil && len(plugins) < validate.MaximumTradePlugins && !slices.ContainsFunc(plugins,
				func(p string) bool { return strings.EqualFold(p, f.GetPlugin()) }) {
				plugins = append(plugins, f.GetPlugin())
			}
		}
	}
	if len(plugins) == 0 {
		return
	}
	t.pluginQueries = append(t.pluginQueries, plugins)
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradePluginQuery{TradePluginQuery: &pb.TradePluginQuery{
		TradeId: t.id, Plugins: plugins}}})
	say("trade %d: asked %s about %d plugin(s)", t.id, t.peer, len(plugins))
}

// answerPlugins reports the plugins the bot's skip rules say it lacks.
func (b *bot) answerPlugins(query *pb.TradePluginQuery) {
	t := &b.trade
	if query.GetTradeId() != t.id || t.phase != phaseOpen {
		return
	}
	var missing []uint32
	var names []string
	for i, plugin := range query.GetPlugins() {
		if slices.ContainsFunc(b.skips, func(r skipRule) bool { return r.form == nil && strings.EqualFold(r.plugin, plugin) }) {
			missing = append(missing, uint32(i))
			names = append(names, plugin)
		}
	}
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradePluginReply{TradePluginReply: &pb.TradePluginReply{
		TradeId: t.id, Missing: missing}}})
	say("trade %d: %s asked about %d plugin(s); reported missing: %s", t.id, t.peer, len(query.GetPlugins()),
		listOrNone(names))
}

func (b *bot) receivePluginReply(reply *pb.TradePluginReply) {
	t := &b.trade
	if reply.GetTradeId() != t.id || t.phase != phaseOpen || len(t.pluginQueries) == 0 {
		return
	}
	query := t.pluginQueries[0]
	t.pluginQueries = t.pluginQueries[1:]
	var names []string
	for _, index := range reply.GetMissing() {
		if int(index) < len(query) {
			names = append(names, query[index])
		}
	}
	say("trade %d: %s lacks: %s", t.id, t.peer, listOrNone(names))
}

func listOrNone(names []string) string {
	if len(names) == 0 {
		return "none"
	}
	return strings.Join(names, ", ")
}

func (b *bot) receiveReport(report *pb.TradeReport) {
	t := &b.trade
	if report.GetTradeId() != t.id || t.phase != phaseOpen || report.GetRevision() != t.ownRevision {
		return
	}
	t.peerSkips, t.reported = report.GetSkipped(), true
	if len(t.peerSkips) == 0 {
		say("trade %d: %s can receive all of revision %d", t.id, t.peer, t.ownRevision)
		return
	}
	say("trade %d: %s cannot receive from revision %d:", t.id, t.peer, t.ownRevision)
	sent := t.sent[t.ownRevision]
	skipped := make(map[int]bool, len(t.peerSkips))
	for _, s := range t.peerSkips {
		skipped[int(s.GetIndex())] = true
		say("    %d %s: %s", s.GetIndex()+1, formatItem(sent[s.GetIndex()]), skipName(s))
	}
	// As the plugin does: a skipped line leaves the offer, which sends a new revision.
	kept := b.offer[:0:0]
	for i, item := range b.offer {
		if !skipped[i] {
			kept = append(kept, item)
		}
	}
	b.offer = kept
	say("dropping them from the offer")
	b.sendOffer()
}

func (b *bot) commit(c *pb.TradeCommit) {
	t := &b.trade
	if c.GetTradeId() != t.id {
		return
	}
	given, ok := t.sent[c.GetOwnRevision()]
	switch {
	case c.GetOwnRevision() == 0:
		given, ok = nil, true
	case c.GetOwnRevision() == t.ownRevision && t.reported:
		given = withoutSkipped(given, t.peerSkips)
	case ok:
		say("the commit names the bot's older revision %d; the peer's report on it is not kept", c.GetOwnRevision())
	}
	say("trade %d: completed with %s (bot revision %d, %s's %d)", t.id, t.peer, c.GetOwnRevision(), t.peer, c.GetPeerRevision())
	if ok {
		say("  the bot gave:")
		printItems(given, nil, "    ")
	} else {
		say("  the bot gave its revision %d, which it no longer keeps", c.GetOwnRevision())
	}
	if c.GetPeerRevision() == t.peerRevision {
		say("  the bot received:")
		printItems(withoutSkipped(t.peerItems, t.ownSkips), nil, "    ")
	} else {
		say("  the commit names %s's revision %d, not the current %d", t.peer, c.GetPeerRevision(), t.peerRevision)
	}
	*t = trade{}
}

func (b *bot) request() {
	if !b.target.located || b.target.slot == 0 {
		say("no target to invite yet")
		return
	}
	if b.trade.phase != phaseIdle {
		say("a trade is already %s", phaseNames[b.trade.phase])
		return
	}
	b.trade = trade{phase: phaseRequesting, peer: b.target.name}
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeRequest{TradeRequest: &pb.TradeRequest{Slot: b.target.slot}}})
	say("asked to trade with %s (%.0f units away)", b.target.name, b.distanceToTarget())
}

func (b *bot) answer(accept bool) {
	t := &b.trade
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeAnswer{TradeAnswer: &pb.TradeAnswer{
		TradeId: t.id, Accept: accept}}})
	if !accept {
		say("trade %d: declined", t.id)
	}
}

func (b *bot) accept() {
	t := &b.trade
	if t.reportedPeer != t.peerRevision {
		say("warning: the bot has not reported on revision %d; the server will drop the acceptance", t.peerRevision)
	}
	t.ownAccepted = true
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeAccept{TradeAccept: &pb.TradeAccept{
		TradeId: t.id, OwnRevision: t.ownRevision, PeerRevision: t.peerRevision}}})
	say("trade %d: the bot accepts its revision %d and %s's %d", t.id, t.ownRevision, t.peer, t.peerRevision)
}

func (b *bot) cancel(reason pb.TradeCloseReason) {
	t := &b.trade
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeCancel{TradeCancel: &pb.TradeCancel{
		TradeId: t.id, Reason: reason}}})
	say("trade %d: cancelled, %s", t.id, closeReasonName(reason))
	*t = trade{}
}

// offerChanged sends the edited offer while a trade is open; otherwise it waits for the next one.
func (b *bot) offerChanged() {
	if b.trade.phase == phaseOpen {
		b.sendOffer()
	}
}

func (b *bot) sendOffer() {
	t := &b.trade
	t.ownRevision++
	items := make([]*pb.TradeItem, len(b.offer))
	for i, item := range b.offer {
		items[i] = proto.CloneOf(item)
	}
	t.sent[t.ownRevision] = items
	delete(t.sent, t.ownRevision-keptRevisions)
	t.peerSkips, t.reported = nil, false
	t.ownAccepted, t.peerAccepted = false, false
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_TradeOffer{TradeOffer: &pb.TradeOffer{
		TradeId: t.id, Revision: t.ownRevision, Items: items}}})
	say("trade %d: sent revision %d with %d line(s)", t.id, t.ownRevision, len(items))
}

// copyPeer replaces the bot's offer with the peer's lines the bot can receive, or only the given ones.
func (b *bot) copyPeer(lines []int) {
	t := &b.trade
	receivable := withoutSkippedIndices(t.peerItems, t.ownSkips)
	var items []*pb.TradeItem
	for i, item := range t.peerItems {
		if receivable[i] && (lines == nil || slices.Contains(lines, i)) {
			items = append(items, proto.CloneOf(item))
		}
	}
	if err := checkOffer(items); err != nil {
		say("cannot copy: %v", err)
		return
	}
	b.offer = items
	say("the bot's offer is now %d line(s) copied from %s", len(items), t.peer)
	b.offerChanged()
}

func checkOffer(items []*pb.TradeItem) error {
	return validate.TradeOffer(&pb.TradeOffer{TradeId: 1, Revision: 1, Items: items})
}

func withoutSkippedIndices(items []*pb.TradeItem, skips []*pb.TradeSkip) []bool {
	receivable := make([]bool, len(items))
	for i := range receivable {
		receivable[i] = true
	}
	for _, s := range skips {
		if int(s.GetIndex()) < len(items) {
			receivable[s.GetIndex()] = false
		}
	}
	return receivable
}

func withoutSkipped(items []*pb.TradeItem, skips []*pb.TradeSkip) []*pb.TradeItem {
	receivable := withoutSkippedIndices(items, skips)
	var kept []*pb.TradeItem
	for i, item := range items {
		if receivable[i] {
			kept = append(kept, item)
		}
	}
	return kept
}

func closeReasonName(r pb.TradeCloseReason) string {
	return strings.ToLower(strings.TrimPrefix(r.String(), "TRADE_CLOSE_REASON_"))
}

func skipName(s *pb.TradeSkip) string {
	name := strings.ToLower(strings.TrimPrefix(s.GetReason().String(), "TRADE_SKIP_REASON_"))
	if s.GetPlugin() != "" {
		name += " (" + s.GetPlugin() + ")"
	}
	return name
}
