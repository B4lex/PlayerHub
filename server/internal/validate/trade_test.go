package validate

import (
	"math"
	"strings"
	"testing"

	"playerhub/server/internal/wire/pb"
)

func validTradeItem() *pb.TradeItem {
	return &pb.TradeItem{Item: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0x12EB7}, Count: 1,
		Effects: []*pb.TradeEffect{{Effect: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0x4605A}, Magnitude: 10,
			Duration: 1, Cost: 40}},
		MaximumCharge: 1500, Charge: 1200.5, Health: 1.6, Name: "Dawnbreaker's cousin", Soul: 0}
}

func offerOf(items ...*pb.TradeItem) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_TradeOffer{TradeOffer: &pb.TradeOffer{
		TradeId: 1, Revision: 1, Items: items}}}
}

func TestTradeOffer(t *testing.T) {
	if err := Message(offerOf(validTradeItem(), &pb.TradeItem{Item: &pb.FormRef{Plugin: "Mod.esp"},
		Count: 1000, Enchantment: &pb.FormRef{Plugin: "Mod.esp", FormId: 2}, Soul: 5})); err != nil {
		t.Errorf("offer rejected: %v", err)
	}
	plain := &pb.TradeItem{Item: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0xF}, Count: math.MaxInt32}
	tempered := &pb.TradeItem{Item: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0xF}, Count: 1000, Health: 1.1}
	if err := Message(offerOf(plain, tempered)); err != nil {
		t.Errorf("large stacks rejected: %v", err)
	}
	if err := Message(offerOf()); err != nil {
		t.Errorf("empty offer rejected: %v", err)
	}
	broken := map[string]func(*pb.TradeItem){
		"no item":                 func(i *pb.TradeItem) { i.Item = nil },
		"empty plugin":            func(i *pb.TradeItem) { i.Item.Plugin = "" },
		"zero count":              func(i *pb.TradeItem) { i.Count = 0 },
		"count over int32":        func(i *pb.TradeItem) { i.Count = math.MaxInt32 + 1 },
		"enchantment and effects": func(i *pb.TradeItem) { i.Enchantment = &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1} },
		"five effects": func(i *pb.TradeItem) {
			for range 4 {
				i.Effects = append(i.Effects, i.Effects[0])
			}
		},
		"effect without form": func(i *pb.TradeItem) { i.Effects[0].Effect = nil },
		"NaN magnitude":       func(i *pb.TradeItem) { i.Effects[0].Magnitude = float32(math.NaN()) },
		"negative magnitude":  func(i *pb.TradeItem) { i.Effects[0].Magnitude = -1 },
		"long duration":       func(i *pb.TradeItem) { i.Effects[0].Duration = 1e6 + 1 },
		"infinite cost":       func(i *pb.TradeItem) { i.Effects[0].Cost = float32(math.Inf(1)) },
		"charge over uint16":  func(i *pb.TradeItem) { i.MaximumCharge = 0x10000 },
		"charge unenchanted": func(i *pb.TradeItem) {
			i.Effects = nil
		},
		"health over 100": func(i *pb.TradeItem) { i.Health = 101 },
		"control in name": func(i *pb.TradeItem) { i.Name = "a\nb" },
		"long name":       func(i *pb.TradeItem) { i.Name = strings.Repeat("x", 129) },
		"soul over grand": func(i *pb.TradeItem) { i.Soul = 6 },
		"1001 enchanted":  func(i *pb.TradeItem) { i.Count = 1001 },
		"1001 named": func(i *pb.TradeItem) {
			i.Effects, i.MaximumCharge, i.Charge, i.Health, i.Count = nil, 0, 0, 0, 1001
		},
	}
	for name, breakItem := range broken {
		item := validTradeItem()
		breakItem(item)
		if Message(offerOf(item)) != ErrTrade {
			t.Errorf("%s accepted", name)
		}
	}
	many := make([]*pb.TradeItem, MaximumTradeItems+1)
	for i := range many {
		many[i] = validTradeItem()
	}
	if Message(offerOf(many...)) != ErrTrade {
		t.Error("16 items accepted")
	}
	if Message(offerOf(many[:MaximumTradeItems]...)) != nil {
		t.Error("15 items rejected")
	}
	for _, offer := range []*pb.TradeOffer{{TradeId: 0, Revision: 1}, {TradeId: 1, Revision: 0}} {
		if Message(&pb.ClientMessage{Message: &pb.ClientMessage_TradeOffer{TradeOffer: offer}}) != ErrTrade {
			t.Errorf("offer %v accepted", offer)
		}
	}
}

func TestTradeReport(t *testing.T) {
	report := func(skipped ...*pb.TradeSkip) *pb.ClientMessage {
		return &pb.ClientMessage{Message: &pb.ClientMessage_TradeReport{TradeReport: &pb.TradeReport{
			TradeId: 1, Revision: 1, Skipped: skipped}}}
	}
	skip := func(index uint32, reason pb.TradeSkipReason, plugin string) *pb.TradeSkip {
		return &pb.TradeSkip{Index: index, Reason: reason, Plugin: plugin}
	}
	if err := Message(report(skip(0, pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, "Mod.esp"),
		skip(14, pb.TradeSkipReason_TRADE_SKIP_REASON_BAD_ENCHANTMENT, ""))); err != nil {
		t.Errorf("report rejected: %v", err)
	}
	for name, s := range map[string]*pb.TradeSkip{
		"index 15":       skip(15, pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, ""),
		"no reason":      skip(1, pb.TradeSkipReason_TRADE_SKIP_REASON_UNSPECIFIED, ""),
		"unknown reason": skip(1, 5, ""),
		"long plugin":    skip(1, pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, strings.Repeat("p", 261)),
		"control":        skip(1, pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, "a\x01"),
	} {
		if Message(report(s)) != ErrTrade {
			t.Errorf("%s accepted", name)
		}
	}
	duplicate := skip(3, pb.TradeSkipReason_TRADE_SKIP_REASON_WRONG_TYPE, "")
	if Message(report(duplicate, duplicate)) != ErrTrade {
		t.Error("a duplicate index accepted")
	}
}

func TestTradePluginQuery(t *testing.T) {
	query := func(plugins ...string) *pb.ClientMessage {
		return &pb.ClientMessage{Message: &pb.ClientMessage_TradePluginQuery{TradePluginQuery: &pb.TradePluginQuery{
			TradeId: 1, Plugins: plugins}}}
	}
	full := make([]string, MaximumTradePlugins)
	for i := range full {
		full[i] = "Mod.esp"
	}
	for name, m := range map[string]*pb.ClientMessage{
		"one":     query("Skyrim.esm"),
		"longest": query(strings.Repeat("p", 260)),
		"full":    query(full...),
	} {
		if err := Message(m); err != nil {
			t.Errorf("%s rejected: %v", name, err)
		}
	}
	for name, m := range map[string]*pb.ClientMessage{
		"empty":       query(),
		"over":        query(append(full, "Mod.esp")...),
		"no name":     query(""),
		"long name":   query(strings.Repeat("p", 261)),
		"control":     query("a\x01"),
		"invalid utf": query("\xff"),
		"no trade": {Message: &pb.ClientMessage_TradePluginQuery{TradePluginQuery: &pb.TradePluginQuery{
			Plugins: []string{"Mod.esp"}}}},
	} {
		if Message(m) != ErrTrade {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestTradePluginReply(t *testing.T) {
	reply := func(id uint32, missing ...uint32) *pb.ClientMessage {
		return &pb.ClientMessage{Message: &pb.ClientMessage_TradePluginReply{TradePluginReply: &pb.TradePluginReply{
			TradeId: id, Missing: missing}}}
	}
	for name, m := range map[string]*pb.ClientMessage{
		"none":    reply(1),
		"several": reply(1, 3, 0, 255),
	} {
		if err := Message(m); err != nil {
			t.Errorf("%s rejected: %v", name, err)
		}
	}
	for name, m := range map[string]*pb.ClientMessage{
		"no trade":  reply(0),
		"index 256": reply(1, 256),
		"duplicate": reply(1, 4, 4),
	} {
		if Message(m) != ErrTrade {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestTradeMessages(t *testing.T) {
	accepted := []*pb.ClientMessage{
		{Message: &pb.ClientMessage_TradeRequest{TradeRequest: &pb.TradeRequest{Slot: 4}}},
		{Message: &pb.ClientMessage_TradeAnswer{TradeAnswer: &pb.TradeAnswer{TradeId: 4}}},
		{Message: &pb.ClientMessage_TradeAccept{TradeAccept: &pb.TradeAccept{TradeId: 4}}},
		{Message: &pb.ClientMessage_TradeCancel{TradeCancel: &pb.TradeCancel{TradeId: 4,
			Reason: pb.TradeCloseReason_TRADE_CLOSE_REASON_COMBAT}}},
	}
	for _, m := range accepted {
		if err := Message(m); err != nil {
			t.Errorf("%v rejected: %v", m, err)
		}
	}
	rejected := []*pb.ClientMessage{
		{Message: &pb.ClientMessage_TradeRequest{TradeRequest: &pb.TradeRequest{}}},
		{Message: &pb.ClientMessage_TradeAnswer{TradeAnswer: &pb.TradeAnswer{}}},
		{Message: &pb.ClientMessage_TradeAccept{TradeAccept: &pb.TradeAccept{}}},
		{Message: &pb.ClientMessage_TradeCancel{TradeCancel: &pb.TradeCancel{Reason: pb.TradeCloseReason_TRADE_CLOSE_REASON_MENU}}},
		{Message: &pb.ClientMessage_TradeCancel{TradeCancel: &pb.TradeCancel{TradeId: 4,
			Reason: pb.TradeCloseReason_TRADE_CLOSE_REASON_EXPIRED}}},
	}
	for _, m := range rejected {
		if Message(m) != ErrTrade {
			t.Errorf("%v accepted", m)
		}
	}
}
