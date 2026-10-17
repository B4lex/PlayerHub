package hub

import (
	"errors"
	"time"

	"playerhub/server/internal/interest"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

var (
	ErrUnknownTrade = errors.New("unknown trade")
	ErrStaleTrade   = errors.New("stale trade message")
)

const (
	requester = 0
	invited   = 1
)

type trade struct {
	id    uint32
	slots [2]uint32
	open  bool
	// The invited player vanished the requester: it is never told, and the invitation expires.
	hidden  bool
	expires time.Time

	revision [2]uint64
	items    [2]int
	// The other side's offer revision this side reported on.
	reported [2]uint64
	accepted [2]bool
	// The plugin counts of each side's queries the other side has not answered, oldest first.
	queries [2][]int
}

func (t *trade) side(slot uint32) int {
	if t.slots[invited] == slot {
		return invited
	}
	return requester
}

func (h *Hub) tradeMember(t *trade, side int) *member {
	if m := h.members[t.slots[side]]; m != nil && m.trade == t {
		return m
	}
	return nil
}

func (h *Hub) sendTrade(m *member, message *pb.ServerMessage) {
	if m != nil && m.outbox != nil {
		m.outbox.AppendGlobal(wire.Marshal(message))
	}
}

func tradeClose(id uint32, reason pb.TradeCloseReason) *pb.ServerMessage {
	return &pb.ServerMessage{Message: &pb.ServerMessage_TradeClose{TradeClose: &pb.TradeClose{TradeId: id, Reason: reason}}}
}

func (h *Hub) inTradeRange(a, b *member, radius float64) bool {
	return a.Placed() && b.Placed() && a.view.Space == b.view.Space && interest.Distance(&a.view, &b.view) <= radius
}

func (h *Hub) requestTrade(m *member, request *pb.TradeRequest) error {
	if !m.listed() {
		return nil
	}
	refuse := func(reason pb.TradeCloseReason, err error) error {
		h.sendTrade(m, tradeClose(0, reason))
		return err
	}
	if !m.tradeRequests.Allow(h.opts.Clock.Now(), tradeRequestsPerSecond, tradeRequestBurst) {
		return refuse(pb.TradeCloseReason_TRADE_CLOSE_REASON_UNAVAILABLE, ErrRateLimited)
	}
	target := h.members[request.GetSlot()]
	if target == nil || !target.listed() || target == m || m.view.Vanished[target.Session] {
		return refuse(pb.TradeCloseReason_TRADE_CLOSE_REASON_UNAVAILABLE, ErrUnknownRecipient)
	}
	if m.trade != nil || target.trade != nil {
		return refuse(pb.TradeCloseReason_TRADE_CLOSE_REASON_BUSY, nil)
	}
	if !h.inTradeRange(m, target, h.opts.TradeRequestRadius) {
		return refuse(pb.TradeCloseReason_TRADE_CLOSE_REASON_OUT_OF_RANGE, nil)
	}
	if m.Transformed() || target.Transformed() {
		return refuse(pb.TradeCloseReason_TRADE_CLOSE_REASON_TRANSFORMED, nil)
	}
	h.nextTrade++
	if h.nextTrade == 0 {
		h.nextTrade++
	}
	t := &trade{id: h.nextTrade, slots: [2]uint32{m.Slot, target.Slot},
		hidden:  target.view.Vanished[m.Session],
		expires: h.opts.Clock.Now().Add(time.Duration(h.opts.TradeInviteSeconds * float64(time.Second)))}
	h.trades[t.id] = t
	m.trade = t
	invite := &pb.ServerMessage{Message: &pb.ServerMessage_TradeInvite{TradeInvite: &pb.TradeInvite{
		TradeId: t.id, Sender: h.speaker(m), Recipient: h.speaker(target)}}}
	h.sendTrade(m, invite)
	if !t.hidden {
		target.trade = t
		h.sendTrade(target, invite)
	}
	return nil
}

func (h *Hub) openTrade(m *member, id uint32, open bool) (*trade, int, error) {
	t := m.trade
	if t == nil || t.id != id {
		return nil, 0, ErrUnknownTrade
	}
	if t.open != open {
		return nil, 0, ErrStaleTrade
	}
	return t, t.side(m.Slot), nil
}

func (h *Hub) answerTrade(m *member, answer *pb.TradeAnswer) error {
	t, side, err := h.openTrade(m, answer.GetTradeId(), false)
	if err != nil {
		return err
	}
	if side != invited {
		return ErrStaleTrade
	}
	if !answer.GetAccept() {
		h.closeTrade(t, pb.TradeCloseReason_TRADE_CLOSE_REASON_DECLINED)
		return nil
	}
	other := h.tradeMember(t, requester)
	if other == nil || !h.inTradeRange(m, other, h.opts.TradeKeepRadius) {
		h.closeTrade(t, pb.TradeCloseReason_TRADE_CLOSE_REASON_OUT_OF_RANGE)
		return nil
	}
	t.open = true
	open := &pb.ServerMessage{Message: &pb.ServerMessage_TradeOpen{TradeOpen: &pb.TradeOpen{TradeId: t.id}}}
	h.sendTrade(other, open)
	h.sendTrade(m, open)
	return nil
}

func (h *Hub) offerTrade(m *member, offer *pb.TradeOffer) error {
	t, side, err := h.openTrade(m, offer.GetTradeId(), true)
	if err != nil {
		return err
	}
	if offer.GetRevision() <= t.revision[side] {
		return ErrStaleTrade
	}
	if !m.tradeOffers.Allow(h.opts.Clock.Now(), tradeOffersPerSecond, tradeOfferBurst) {
		return ErrRateLimited
	}
	t.revision[side], t.items[side] = offer.GetRevision(), len(offer.GetItems())
	t.accepted = [2]bool{}
	h.sendTrade(h.tradeMember(t, 1-side), &pb.ServerMessage{Message: &pb.ServerMessage_TradeOffer{TradeOffer: offer}})
	return nil
}

func (h *Hub) reportTrade(m *member, report *pb.TradeReport) error {
	t, side, err := h.openTrade(m, report.GetTradeId(), true)
	if err != nil {
		return err
	}
	other := 1 - side
	if report.GetRevision() != t.revision[other] || t.reported[side] == report.GetRevision() {
		return ErrStaleTrade
	}
	for _, skip := range report.GetSkipped() {
		if int(skip.GetIndex()) >= t.items[other] {
			return ErrStaleTrade
		}
	}
	t.reported[side] = report.GetRevision()
	h.sendTrade(h.tradeMember(t, other), &pb.ServerMessage{Message: &pb.ServerMessage_TradeReport{TradeReport: report}})
	return nil
}

func (h *Hub) queryTradePlugins(m *member, query *pb.TradePluginQuery) error {
	t, side, err := h.openTrade(m, query.GetTradeId(), true)
	if err != nil {
		return err
	}
	if len(t.queries[side]) >= tradePendingPluginQueries ||
		!m.tradeOffers.Allow(h.opts.Clock.Now(), tradeOffersPerSecond, tradeOfferBurst) {
		return ErrRateLimited
	}
	t.queries[side] = append(t.queries[side], len(query.GetPlugins()))
	h.sendTrade(h.tradeMember(t, 1-side),
		&pb.ServerMessage{Message: &pb.ServerMessage_TradePluginQuery{TradePluginQuery: query}})
	return nil
}

func (h *Hub) replyTradePlugins(m *member, reply *pb.TradePluginReply) error {
	t, side, err := h.openTrade(m, reply.GetTradeId(), true)
	if err != nil {
		return err
	}
	other := 1 - side
	if len(t.queries[other]) == 0 {
		return ErrStaleTrade
	}
	for _, index := range reply.GetMissing() {
		if int(index) >= t.queries[other][0] {
			return ErrStaleTrade
		}
	}
	t.queries[other] = t.queries[other][1:]
	h.sendTrade(h.tradeMember(t, other),
		&pb.ServerMessage{Message: &pb.ServerMessage_TradePluginReply{TradePluginReply: reply}})
	return nil
}

func (h *Hub) acceptTrade(m *member, accept *pb.TradeAccept) error {
	t, side, err := h.openTrade(m, accept.GetTradeId(), true)
	if err != nil {
		return err
	}
	other := 1 - side
	// An offer clears both acceptances, so an acceptance always names the current revisions.
	if accept.GetOwnRevision() != t.revision[side] || accept.GetPeerRevision() != t.revision[other] ||
		t.reported[side] != t.revision[other] {
		return ErrStaleTrade
	}
	t.accepted[side] = true
	peer := h.tradeMember(t, other)
	if !t.accepted[other] {
		h.sendTrade(peer, &pb.ServerMessage{Message: &pb.ServerMessage_TradeAccept{TradeAccept: accept}})
		return nil
	}
	h.finishTrade(t)
	for s, party := range [2]*member{h.tradeMember(t, requester), h.tradeMember(t, invited)} {
		h.sendTrade(party, &pb.ServerMessage{Message: &pb.ServerMessage_TradeCommit{TradeCommit: &pb.TradeCommit{
			TradeId: t.id, OwnRevision: t.revision[s], PeerRevision: t.revision[1-s]}}})
	}
	h.forgetTrade(t)
	return nil
}

func (h *Hub) cancelTrade(m *member, cancel *pb.TradeCancel) error {
	t := m.trade
	if t == nil || t.id != cancel.GetTradeId() {
		return ErrUnknownTrade
	}
	h.closeTrade(t, cancel.GetReason())
	return nil
}

func (h *Hub) finishTrade(t *trade) {
	h.opts.Logger.Info("trade completed", "trade", t.id, "requester", t.slots[requester], "invited", t.slots[invited],
		"requesterItems", t.items[requester], "invitedItems", t.items[invited])
}

func (h *Hub) closeTrade(t *trade, reason pb.TradeCloseReason) {
	close := tradeClose(t.id, reason)
	h.sendTrade(h.tradeMember(t, requester), close)
	h.sendTrade(h.tradeMember(t, invited), close)
	h.forgetTrade(t)
}

func (h *Hub) forgetTrade(t *trade) {
	for side := range t.slots {
		if m := h.tradeMember(t, side); m != nil {
			m.trade = nil
		}
	}
	delete(h.trades, t.id)
}

func (h *Hub) checkTradeRange(m *member) {
	t := m.trade
	if t == nil {
		return
	}
	other := h.members[t.slots[1-t.side(m.Slot)]]
	if other == nil || !h.inTradeRange(m, other, h.opts.TradeKeepRadius) {
		h.closeTrade(t, pb.TradeCloseReason_TRADE_CLOSE_REASON_OUT_OF_RANGE)
	}
}

// A beast form has no inventory to trade from, so a transformation ends the trade.
func (h *Hub) checkTradeForm(m *member) {
	if m.trade != nil && m.Transformed() {
		h.closeTrade(m.trade, pb.TradeCloseReason_TRADE_CLOSE_REASON_TRANSFORMED)
	}
}

func (h *Hub) expireTrades(now time.Time) {
	for _, t := range h.trades {
		if !t.open && now.After(t.expires) {
			h.closeTrade(t, pb.TradeCloseReason_TRADE_CLOSE_REASON_EXPIRED)
		}
	}
}
