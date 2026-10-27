package main

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"

	"playerhub/server/internal/wire/pb"
)

// A plugin no game loads, for an item the receiver must skip with MISSING_ITEM and the plugin.
const missingPlugin = "TradeBotMissing.esp"

func testKit() []*pb.TradeItem {
	skyrim := func(id uint32) *pb.FormRef { return &pb.FormRef{Plugin: "Skyrim.esm", FormId: id} }
	return []*pb.TradeItem{
		{Item: skyrim(0x00000F), Count: 500},                   // Gold
		{Item: skyrim(0x00000A), Count: 5},                     // Lockpick
		{Item: skyrim(0x012EB7), Count: 1},                     // Iron Sword
		{Item: skyrim(0x013989), Count: 1, Health: 1.2},        // Steel Sword, Superior
		{Item: skyrim(0x01397E), Count: 1, Name: "Bot Dagger"}, // Iron Dagger
		{Item: skyrim(0x01397D), Count: 24},                    // Iron Arrow
		{Item: &pb.FormRef{Plugin: missingPlugin, FormId: 0x800}, Count: 1},
	}
}

// parseItem reads [<plugin>:<form id>] [count] [key=value ...]; without a form it is a player-made potion or poison.
func parseItem(args []string) (*pb.TradeItem, error) {
	if len(args) == 0 {
		return nil, errors.New("usage: add [<plugin>:<form id>] [count] [ench=<form>] [effect=<form>/<magnitude>/<area>/<duration>/<cost>] [charge=<n>] [maxcharge=<n>] [health=<n>] [name=<text>] [soul=<0..5>] [poison]")
	}
	item := &pb.TradeItem{Count: 1}
	rest := args
	var err error
	if strings.Contains(rest[0], ":") && !strings.Contains(rest[0], "=") {
		if item.Item, err = parseForm(rest[0]); err != nil {
			return nil, err
		}
		rest = rest[1:]
	}
	if len(rest) > 0 && !strings.Contains(rest[0], "=") {
		count, err := strconv.ParseUint(rest[0], 10, 32)
		if err != nil {
			return nil, fmt.Errorf("count %q: %w", rest[0], err)
		}
		item.Count = uint32(count)
		rest = rest[1:]
	}
	for _, arg := range rest {
		if arg == "poison" {
			item.Poison = true
			continue
		}
		key, value, ok := strings.Cut(arg, "=")
		if !ok {
			return nil, fmt.Errorf("%q is not key=value", arg)
		}
		switch key {
		case "ench":
			if item.Enchantment, err = parseForm(value); err != nil {
				return nil, err
			}
		case "effect":
			effect, err := parseEffect(value)
			if err != nil {
				return nil, err
			}
			item.Effects = append(item.Effects, effect)
		case "charge":
			item.Charge, err = parseFloat(value)
		case "maxcharge":
			var n uint64
			n, err = strconv.ParseUint(value, 10, 32)
			item.MaximumCharge = uint32(n)
		case "health":
			item.Health, err = parseFloat(value)
		case "name":
			item.Name = value
		case "soul":
			var n uint64
			n, err = strconv.ParseUint(value, 10, 32)
			item.Soul = uint32(n)
		default:
			return nil, fmt.Errorf("unknown key %q", key)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: %w", key, err)
		}
	}
	return item, nil
}

func parseForm(s string) (*pb.FormRef, error) {
	plugin, id, ok := strings.Cut(s, ":")
	if !ok || plugin == "" {
		return nil, fmt.Errorf("form %q is not <plugin>:<hex id>", s)
	}
	n, err := strconv.ParseUint(strings.TrimPrefix(strings.ToLower(id), "0x"), 16, 32)
	if err != nil || n > 0xFFFFFF {
		return nil, fmt.Errorf("form %q: the ID must be the plugin-local hex ID, at most FFFFFF", s)
	}
	return &pb.FormRef{Plugin: plugin, FormId: uint32(n)}, nil
}

func parseEffect(s string) (*pb.TradeEffect, error) {
	parts := strings.Split(s, "/")
	if len(parts) != 5 {
		return nil, fmt.Errorf("effect %q is not <form>/<magnitude>/<area>/<duration>/<cost>", s)
	}
	form, err := parseForm(parts[0])
	if err != nil {
		return nil, err
	}
	var values [4]float64
	for i, part := range parts[1:] {
		if values[i], err = strconv.ParseFloat(part, 64); err != nil {
			return nil, fmt.Errorf("effect %q: %w", s, err)
		}
	}
	return &pb.TradeEffect{Effect: form, Magnitude: float32(values[0]), Area: uint32(values[1]),
		Duration: uint32(values[2]), Cost: float32(values[3])}, nil
}

func parseFloat(s string) (float32, error) {
	v, err := strconv.ParseFloat(s, 32)
	if err == nil && (math.IsNaN(v) || math.IsInf(v, 0)) {
		err = errors.New("not finite")
	}
	return float32(v), err
}

func formatForm(f *pb.FormRef) string {
	if f == nil {
		return "player-made"
	}
	return fmt.Sprintf("%s:%06X", f.GetPlugin(), f.GetFormId())
}

func formatItem(item *pb.TradeItem) string {
	parts := []string{fmt.Sprintf("%s x%d", formatForm(item.GetItem()), item.GetCount())}
	if e := item.GetEnchantment(); e != nil {
		parts = append(parts, "ench="+formatForm(e))
	}
	for _, e := range item.GetEffects() {
		parts = append(parts, fmt.Sprintf("effect=%s/%g/%d/%d/%g", formatForm(e.GetEffect()), e.GetMagnitude(),
			e.GetArea(), e.GetDuration(), e.GetCost()))
	}
	if item.GetMaximumCharge() != 0 || item.GetCharge() != 0 {
		parts = append(parts, fmt.Sprintf("charge=%g maxcharge=%d", item.GetCharge(), item.GetMaximumCharge()))
	}
	if item.GetHealth() != 0 {
		parts = append(parts, fmt.Sprintf("health=%.2f", item.GetHealth()))
	}
	if item.GetName() != "" {
		parts = append(parts, fmt.Sprintf("name=%q", item.GetName()))
	}
	if item.GetSoul() != 0 {
		parts = append(parts, fmt.Sprintf("soul=%d", item.GetSoul()))
	}
	if item.GetPoison() {
		parts = append(parts, "poison")
	}
	return strings.Join(parts, " ")
}

func printItems(items []*pb.TradeItem, skips []*pb.TradeSkip, indent string) {
	if len(items) == 0 {
		say("%s(nothing)", indent)
		return
	}
	reasons := make(map[int]string, len(skips))
	for _, s := range skips {
		reasons[int(s.GetIndex())] = skipName(s)
	}
	for i, item := range items {
		line := fmt.Sprintf("%s%d %s", indent, i+1, formatItem(item))
		if reason, ok := reasons[i]; ok {
			line += "  [skipped: " + reason + "]"
		}
		say("%s", line)
	}
}

// skipRule makes the bot report peer lines as its game would if it lacked a plugin or a form.
type skipRule struct {
	plugin string
	form   *pb.FormRef
	reason pb.TradeSkipReason
}

func (r skipRule) String() string {
	if r.form == nil {
		return "plugin " + r.plugin
	}
	return fmt.Sprintf("form %s as %s", formatForm(r.form), skipName(&pb.TradeSkip{Reason: r.reason}))
}

func sameForm(a, b *pb.FormRef) bool {
	return a != nil && b != nil && a.GetFormId() == b.GetFormId() && strings.EqualFold(a.GetPlugin(), b.GetPlugin())
}

// match follows Forms::ResolveItem: the item first, then its enchantment and effects.
func (r skipRule) match(item *pb.TradeItem) (*pb.TradeSkip, bool) {
	magic := append([]*pb.FormRef{item.GetEnchantment()}, effectForms(item)...)
	if r.form == nil {
		if strings.EqualFold(item.GetItem().GetPlugin(), r.plugin) {
			return &pb.TradeSkip{Reason: pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, Plugin: r.plugin}, true
		}
		for _, f := range magic {
			if f != nil && strings.EqualFold(f.GetPlugin(), r.plugin) {
				return &pb.TradeSkip{Reason: pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_EFFECT, Plugin: r.plugin}, true
			}
		}
		return nil, false
	}
	if sameForm(item.GetItem(), r.form) {
		return &pb.TradeSkip{Reason: r.reason}, true
	}
	for _, f := range magic {
		if sameForm(f, r.form) {
			return &pb.TradeSkip{Reason: r.reason}, true
		}
	}
	return nil, false
}

func effectForms(item *pb.TradeItem) []*pb.FormRef {
	forms := make([]*pb.FormRef, len(item.GetEffects()))
	for i, e := range item.GetEffects() {
		forms[i] = e.GetEffect()
	}
	return forms
}

func (b *bot) reportSkips(items []*pb.TradeItem) []*pb.TradeSkip {
	var skips []*pb.TradeSkip
	for i, item := range items {
		for _, rule := range b.skips {
			if skip, ok := rule.match(item); ok {
				skip.Index = uint32(i)
				skips = append(skips, skip)
				break
			}
		}
	}
	return skips
}

func parseSkipReason(s string) (pb.TradeSkipReason, error) {
	switch s {
	case "missing":
		return pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_ITEM, nil
	case "wrong":
		return pb.TradeSkipReason_TRADE_SKIP_REASON_WRONG_TYPE, nil
	case "effect":
		return pb.TradeSkipReason_TRADE_SKIP_REASON_MISSING_EFFECT, nil
	case "enchantment":
		return pb.TradeSkipReason_TRADE_SKIP_REASON_BAD_ENCHANTMENT, nil
	}
	return 0, fmt.Errorf("reason %q is not missing, wrong, effect, or enchantment", s)
}
