package hub

import (
	"strings"
	"testing"

	"playerhub/server/internal/wire/pb"
)

func tradeRequest(slot uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeRequest{TradeRequest: &pb.TradeRequest{Slot: slot}}}
}

func tradeAnswer(id uint32, accept bool) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeAnswer{TradeAnswer: &pb.TradeAnswer{TradeId: id, Accept: accept}}}
}

func tradeOffer(id uint32, revision uint64, items int) *pb.ClientMessage {
	offer := &pb.TradeOffer{TradeId: id, Revision: revision}
	for range items {
		offer.Items = append(offer.Items, &pb.TradeItem{Item: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0xF}, Count: 1})
	}
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeOffer{TradeOffer: offer}}
}

func tradeReport(id uint32, revision uint64, skipped ...uint32) *pb.ClientMessage {
	report := &pb.TradeReport{TradeId: id, Revision: revision}
	for _, index := range skipped {
		report.Skipped = append(report.Skipped, &pb.TradeSkip{Index: index,
			Reason: pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, Plugin: "Mod.esp"})
	}
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeReport{TradeReport: report}}
}

func tradePluginQuery(id uint32, plugins ...string) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradePluginQuery{TradePluginQuery: &pb.TradePluginQuery{
		TradeId: id, Plugins: plugins}}}
}

func tradePluginReply(id uint32, missing ...uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradePluginReply{TradePluginReply: &pb.TradePluginReply{
		TradeId: id, Missing: missing}}}
}

func tradeAccept(id uint32, own, peer uint64) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeAccept{TradeAccept: &pb.TradeAccept{
		TradeId: id, OwnRevision: own, PeerRevision: peer}}}
}

func tradeCancel(id uint32, reason pb.TradeCloseReason) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeCancel{TradeCancel: &pb.TradeCancel{TradeId: id, Reason: reason}}}
}

// traders places two players 100 units apart, each of them seeing the other.
func traders(t *testing.T) (*fixture, uint32, uint32) {
	f := newFixture(t)
	a, b := f.connectAs("Lydia"), f.connectAs("Serana")
	f.placeAt(a, 1, 0)
	f.placeAt(b, 1, 100)
	f.received(a)
	f.received(b)
	return f, a, b
}

func (f *fixture) openTrade(id, a, b uint32) {
	f.t.Helper()
	f.send(a, tradeRequest(b))
	f.send(b, tradeAnswer(id, true))
	f.received(a)
	f.received(b)
}

func TestTradeCommitsWhenBothAcceptTheSameOffers(t *testing.T) {
	f, a, b := traders(t)
	f.send(a, tradeRequest(b))
	expect(t, f.received(a), "trade invite 1 1>2")
	expect(t, f.received(b), "trade invite 1 1>2")
	f.send(b, tradeAnswer(1, true))
	expect(t, f.received(a), "trade open 1")
	expect(t, f.received(b), "trade open 1")

	f.send(a, tradeOffer(1, 1, 2))
	expect(t, f.received(b), "trade offer 1 rev 1 items 2")
	f.send(b, tradeReport(1, 1, 1))
	expect(t, f.received(a), "trade report 1 rev 1 skipped 1")
	f.send(a, tradeAccept(1, 1, 0))
	expect(t, f.received(b), "trade accept 1 own 1 peer 0")
	f.send(b, tradeAccept(1, 0, 1))
	expect(t, f.received(a), "trade commit 1 own 1 peer 0")
	expect(t, f.received(b), "trade commit 1 own 0 peer 1")

	// Both are free for another trade.
	f.send(b, tradeRequest(a))
	expect(t, f.received(a), "trade invite 2 2>1")
}

func TestTradeOfferClearsAcceptances(t *testing.T) {
	f, a, b := traders(t)
	f.openTrade(1, a, b)
	f.send(a, tradeOffer(1, 1, 1))
	f.send(b, tradeReport(1, 1))
	f.send(a, tradeAccept(1, 1, 0))
	f.send(b, tradeOffer(1, 1, 1))
	f.send(a, tradeReport(1, 1))
	f.send(b, tradeAccept(1, 1, 1))
	expect(t, f.received(a), "trade report 1 rev 1 skipped", "trade offer 1 rev 1 items 1", "trade accept 1 own 1 peer 1")
	expect(t, f.received(b), "trade offer 1 rev 1 items 1", "trade accept 1 own 1 peer 0", "trade report 1 rev 1 skipped")
	f.reject(a, tradeAccept(1, 1, 0), ErrStaleTrade)
	f.send(a, tradeAccept(1, 1, 1))
	expect(t, f.received(a), "trade commit 1 own 1 peer 1")
}

func TestTradeAcceptNeedsCurrentRevisionsAndReport(t *testing.T) {
	f, a, b := traders(t)
	f.openTrade(1, a, b)
	f.send(a, tradeOffer(1, 1, 1))
	f.reject(b, tradeAccept(1, 0, 1), ErrStaleTrade)
	f.reject(b, tradeReport(1, 2), ErrStaleTrade)
	f.reject(b, tradeReport(1, 1, 1), ErrStaleTrade)
	f.send(b, tradeReport(1, 1, 0))
	f.reject(b, tradeReport(1, 1), ErrStaleTrade)
	f.reject(b, tradeAccept(1, 1, 1), ErrStaleTrade)
	f.reject(a, tradeOffer(1, 1, 0), ErrStaleTrade)
	f.send(b, tradeAccept(1, 0, 1))
	f.send(a, tradeOffer(1, 3, 0))
	f.reject(b, tradeAccept(1, 0, 1), ErrStaleTrade)
	f.reject(b, tradeAccept(1, 0, 3), ErrStaleTrade)
}

func TestTradePluginQueriesAreAnsweredInOrder(t *testing.T) {
	f, a, b := traders(t)
	f.reject(a, tradePluginQuery(1, "Mod.esp"), ErrUnknownTrade)
	f.send(a, tradeRequest(b))
	f.received(a)
	f.received(b)
	f.reject(a, tradePluginQuery(1, "Mod.esp"), ErrStaleTrade)
	f.send(b, tradeAnswer(1, true))
	f.received(a)
	f.received(b)

	f.reject(b, tradePluginReply(1), ErrStaleTrade)
	f.send(a, tradePluginQuery(1, "Skyrim.esm", "Mod.esp"))
	f.send(a, tradePluginQuery(1, "Other.esp"))
	expect(t, f.received(b), "trade plugins 1 query Skyrim.esm Mod.esp", "trade plugins 1 query Other.esp")
	// The answer to a player's own query is not its to give.
	f.reject(a, tradePluginReply(1), ErrStaleTrade)
	f.reject(b, tradePluginReply(1, 2), ErrStaleTrade)
	f.send(b, tradePluginReply(1, 1))
	f.reject(b, tradePluginReply(1, 1), ErrStaleTrade)
	f.send(b, tradePluginReply(1, 0))
	expect(t, f.received(a), "trade plugins 1 missing [1]", "trade plugins 1 missing [0]")
	f.reject(b, tradePluginReply(1), ErrStaleTrade)

	f.send(b, tradePluginQuery(1, "Dawnguard.esm"))
	f.send(a, tradePluginReply(1))
	expect(t, f.received(a), "trade plugins 1 query Dawnguard.esm")
	expect(t, f.received(b), "trade plugins 1 missing []")
}

func TestTradePluginQueriesAreLimited(t *testing.T) {
	f, a, b := traders(t)
	f.openTrade(1, a, b)
	for range tradePendingPluginQueries {
		f.send(a, tradePluginQuery(1, "Mod.esp"))
	}
	f.reject(a, tradePluginQuery(1, "Mod.esp"), ErrRateLimited)
	f.send(b, tradePluginReply(1))
	f.send(a, tradePluginQuery(1, "Mod.esp"))
}

func TestTradeRequestRefusals(t *testing.T) {
	f, a, b := traders(t)
	c := f.connectAs("Aela")
	f.placeAt(c, 1, 1000)
	f.received(a)
	f.received(c)
	f.reject(a, tradeRequest(a), ErrUnknownRecipient)
	f.reject(a, tradeRequest(99), ErrUnknownRecipient)
	f.send(a, tradeRequest(c))
	expect(t, f.received(a), "trade close 0 UNAVAILABLE", "trade close 0 UNAVAILABLE", "trade close 0 OUT_OF_RANGE")
	f.reject(a, tradeRequest(b), ErrRateLimited)
	f.clock.Advance(2)
	f.send(a, tradeRequest(b))
	f.placeAt(c, 2, 200)
	f.received(c)
	f.send(c, tradeRequest(b))
	f.send(c, tradeRequest(a))
	expect(t, f.received(c), "trade close 0 BUSY", "trade close 0 BUSY")
}

func TestTradeAnswerAndCancel(t *testing.T) {
	f, a, b := traders(t)
	f.send(a, tradeRequest(b))
	f.reject(a, tradeAnswer(1, true), ErrStaleTrade)
	f.reject(b, tradeAnswer(2, true), ErrUnknownTrade)
	f.reject(b, tradeOffer(1, 1, 0), ErrStaleTrade)
	f.send(b, tradeAnswer(1, false))
	expect(t, f.received(a), "trade invite 1 1>2", "trade close 1 DECLINED")
	expect(t, f.received(b), "trade invite 1 1>2", "trade close 1 DECLINED")

	f.clock.Advance(2)
	f.openTrade(2, a, b)
	f.send(b, tradeCancel(2, pb.TradeCloseReason_TRADE_CLOSE_REASON_MENU))
	expect(t, f.received(a), "trade close 2 MENU")
	expect(t, f.received(b), "trade close 2 MENU")
	f.reject(b, tradeCancel(2, pb.TradeCloseReason_TRADE_CLOSE_REASON_MENU), ErrUnknownTrade)
}

func TestTradeExpiresAndHidesFromAVanishingPlayer(t *testing.T) {
	f, a, b := traders(t)
	f.send(b, vanish(1, true))
	f.send(a, tradeRequest(b))
	expect(t, f.received(a), "trade invite 1 1>2")
	expect(t, f.received(b), "leave 1 EXCLUDED")
	f.reject(b, tradeAnswer(1, true), ErrUnknownTrade)

	c := f.connectAs("Aela")
	f.placeAt(c, 1, 50)
	f.send(c, tradeRequest(b))
	f.clock.Advance(31)
	f.hub.UpdateInterest()
	expect(t, f.received(a), "roster +3 Aela", "enter 3 state 1", "nearby +2 +3", "trade close 1 EXPIRED")
	received := f.received(c)
	expect(t, received[len(received)-3:], "trade invite 2 3>2", "nearby +1 +2", "trade close 2 EXPIRED")
}

func TestTradeClosesWhenPlayersPartOrLeave(t *testing.T) {
	f, a, b := traders(t)
	f.openTrade(1, a, b)
	f.send(a, checkpointAt(2, 900))
	expect(t, tradeLines(f.received(b)))
	f.send(a, checkpointAt(3, 1200))
	expect(t, tradeLines(f.received(a)), "trade close 1 OUT_OF_RANGE")
	expect(t, tradeLines(f.received(b)), "trade close 1 OUT_OF_RANGE")

	f.send(a, checkpointAt(4, 0))
	f.clock.Advance(2)
	f.openTrade(2, a, b)
	f.send(b, exit())
	expect(t, tradeLines(f.received(a)), "trade close 2 OUT_OF_RANGE")

	f.placeAt(b, 1, 100)
	f.clock.Advance(2)
	f.openTrade(3, b, a)
	f.hub.Disconnect(b)
	expect(t, tradeLines(f.received(a)), "trade close 3 DISCONNECTED")
}

func TestTradeNeedsBothPlayersInTheirOwnForm(t *testing.T) {
	f, a, b := traders(t)
	f.send(b, shapeshift(2, pb.Shapeshift_SHAPESHIFT_WEREWOLF))
	f.send(a, tradeRequest(b))
	expect(t, tradeLines(f.received(a)), "trade close 0 TRANSFORMED")
	f.send(b, shapeshift(3, pb.Shapeshift_SHAPESHIFT_NONE))

	f.clock.Advance(2)
	f.send(a, tradeRequest(b))
	f.send(a, shapeshift(2, pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD))
	f.reject(b, tradeAnswer(1, true), ErrUnknownTrade)
	expect(t, tradeLines(f.received(a)), "trade invite 1 1>2", "trade close 1 TRANSFORMED")
	expect(t, tradeLines(f.received(b)), "trade invite 1 1>2", "trade close 1 TRANSFORMED")
	f.send(a, shapeshift(3, pb.Shapeshift_SHAPESHIFT_NONE))

	f.clock.Advance(2)
	f.openTrade(2, a, b)
	f.send(b, shapeshift(4, pb.Shapeshift_SHAPESHIFT_WEREBEAR))
	expect(t, tradeLines(f.received(a)), "trade close 2 TRANSFORMED")
	expect(t, tradeLines(f.received(b)), "trade close 2 TRANSFORMED")
	f.reject(b, tradeCancel(2, pb.TradeCloseReason_TRADE_CLOSE_REASON_TRANSFORMED), ErrUnknownTrade)
}

func tradeLines(lines []string) []string {
	var trades []string
	for _, line := range lines {
		if strings.HasPrefix(line, "trade ") {
			trades = append(trades, line)
		}
	}
	return trades
}
