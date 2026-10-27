package validate

import (
	"errors"
	"math"
	"unicode/utf8"

	"playerhub/server/internal/wire/pb"
)

const (
	MaximumTradeItems       = 15
	MaximumTradeEffects     = 4
	MaximumTradeCount       = math.MaxInt32
	MaximumTradeInstances   = 1000
	MaximumTradeEffectValue = 1e6
	MaximumTradeEffectCost  = 1e7
	MaximumTradeCharge      = 0xFFFF
	MaximumTradeHealth      = 100
	MaximumTradeSoul        = 5
	MaximumTradePlugins     = 256
)

var ErrTrade = errors.New("invalid trade message")

func trade(m *pb.ClientMessage) (bool, error) {
	switch body := m.Message.(type) {
	case *pb.ClientMessage_TradeRequest:
		return true, check(body.TradeRequest.GetSlot() != 0)
	case *pb.ClientMessage_TradeAnswer:
		return true, check(body.TradeAnswer.GetTradeId() != 0)
	case *pb.ClientMessage_TradeOffer:
		return true, TradeOffer(body.TradeOffer)
	case *pb.ClientMessage_TradeReport:
		return true, TradeReport(body.TradeReport)
	case *pb.ClientMessage_TradeAccept:
		return true, check(body.TradeAccept.GetTradeId() != 0)
	case *pb.ClientMessage_TradePluginQuery:
		return true, TradePluginQuery(body.TradePluginQuery)
	case *pb.ClientMessage_TradePluginReply:
		return true, TradePluginReply(body.TradePluginReply)
	case *pb.ClientMessage_TradeCancel:
		c := body.TradeCancel
		switch c.GetReason() {
		case pb.TradeCloseReason_TRADE_CLOSE_REASON_CANCELLED, pb.TradeCloseReason_TRADE_CLOSE_REASON_COMBAT,
			pb.TradeCloseReason_TRADE_CLOSE_REASON_MENU, pb.TradeCloseReason_TRADE_CLOSE_REASON_FAILED,
			pb.TradeCloseReason_TRADE_CLOSE_REASON_TRANSFORMED:
			return true, check(c.GetTradeId() != 0)
		}
		return true, ErrTrade
	}
	return false, nil
}

func check(ok bool) error {
	if !ok {
		return ErrTrade
	}
	return nil
}

func TradeOffer(o *pb.TradeOffer) error {
	if o.GetTradeId() == 0 || o.GetRevision() == 0 || len(o.GetItems()) > MaximumTradeItems {
		return ErrTrade
	}
	for _, item := range o.GetItems() {
		if !tradeItem(item) {
			return ErrTrade
		}
	}
	return nil
}

func tradeItem(i *pb.TradeItem) bool {
	if i.GetCount() == 0 || i.GetCount() > MaximumTradeCount || !tradeEffects(i.GetEffects()) {
		return false
	}
	effects := i.GetEffects()
	if i.Item == nil {
		return len(effects) != 0 && i.Enchantment == nil && i.GetMaximumCharge() == 0 && i.GetCharge() == 0 &&
			i.GetHealth() == 0 && i.GetName() == "" && i.GetSoul() == 0
	}
	if !formRef(i.Item) || (i.Enchantment != nil && len(effects) != 0) ||
		(i.Enchantment != nil && !formRef(i.Enchantment)) {
		return false
	}
	enchanted := i.Enchantment != nil || len(effects) != 0
	if i.GetMaximumCharge() > MaximumTradeCharge || !between(i.GetCharge(), 0, MaximumTradeCharge) ||
		(!enchanted && (i.GetMaximumCharge() != 0 || i.GetCharge() != 0)) {
		return false
	}
	extra := enchanted || i.GetHealth() != 0 || i.GetName() != "" || i.GetSoul() != 0
	return between(i.GetHealth(), 0, MaximumTradeHealth) && name(i.GetName()) && i.GetSoul() <= MaximumTradeSoul &&
		(!extra || i.GetCount() <= MaximumTradeInstances)
}

func tradeEffects(effects []*pb.TradeEffect) bool {
	if len(effects) > MaximumTradeEffects {
		return false
	}
	for _, e := range effects {
		if e.Effect == nil || !formRef(e.Effect) || !between(e.GetMagnitude(), 0, MaximumTradeEffectValue) ||
			e.GetArea() > MaximumTradeEffectValue || e.GetDuration() > MaximumTradeEffectValue ||
			!between(e.GetCost(), 0, MaximumTradeEffectCost) {
			return false
		}
	}
	return true
}

func TradeReport(r *pb.TradeReport) error {
	if r.GetTradeId() == 0 || r.GetRevision() == 0 || len(r.GetSkipped()) > MaximumTradeItems {
		return ErrTrade
	}
	var seen uint32
	for _, skip := range r.GetSkipped() {
		index := skip.GetIndex()
		if index >= MaximumTradeItems || seen&(1<<index) != 0 || skip.GetReason() == 0 ||
			skip.GetReason() > pb.TradeSkipReason_TRADE_SKIP_REASON_BAD_ENCHANTMENT ||
			len(skip.GetPlugin()) > MaximumPluginNameBytes || !utf8.ValidString(skip.GetPlugin()) ||
			hasControl(skip.GetPlugin()) {
			return ErrTrade
		}
		seen |= 1 << index
	}
	return nil
}

func TradePluginQuery(q *pb.TradePluginQuery) error {
	plugins := q.GetPlugins()
	if q.GetTradeId() == 0 || len(plugins) == 0 || len(plugins) > MaximumTradePlugins {
		return ErrTrade
	}
	for _, plugin := range plugins {
		if len(plugin) == 0 || len(plugin) > MaximumPluginNameBytes || !utf8.ValidString(plugin) || hasControl(plugin) {
			return ErrTrade
		}
	}
	return nil
}

func TradePluginReply(r *pb.TradePluginReply) error {
	if r.GetTradeId() == 0 || len(r.GetMissing()) > MaximumTradePlugins {
		return ErrTrade
	}
	var seen [MaximumTradePlugins]bool
	for _, index := range r.GetMissing() {
		if index >= MaximumTradePlugins || seen[index] {
			return ErrTrade
		}
		seen[index] = true
	}
	return nil
}
