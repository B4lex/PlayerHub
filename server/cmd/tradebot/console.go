package main

import (
	"fmt"
	"strconv"
	"strings"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire/pb"
)

const help = `Trade:
  invite                      ask the target to trade
  accept                      accept the invitation, or the exchange while open
  decline                     decline the invitation, or withdraw the bot's own
  cancel [cancelled|combat|menu|failed]
                              close the trade with that reason (default cancelled)
  auto answer|accept on|off   accept invitations / the exchange on their own
Offer (kept between trades; sent at once while a trade is open):
  add <plugin>:<id> [count] [ench=<form>] [effect=<form>/<mag>/<area>/<dur>/<cost>]
      [charge=<n>] [maxcharge=<n>] [health=<n>] [name="<text>"] [soul=<0..5>]
  count <line> <count>        change a line's count
  remove <line>               remove a line
  clear                       empty the offer
  kit                         add the test kit (vanilla items and one from a missing plugin)
  copy [line...]              offer the target's lines back (all, or the given ones)
  mirror on|off               copy the target's offer whenever it changes
Reports on the target's offers:
  skip plugin <name>          act as if <name> is not installed
  skip form <form> missing|wrong|effect|enchantment
                              report a form with that reason
  skip clear                  report everything as receivable
  skip                        list the rules
Movement:
  follow | stay | leave       follow the target, stand still, or run past keep_radius
  target <name>               follow another player
Other:
  status | help | quit`

// command runs one console line and returns false to quit.
func (b *bot) command(line string) bool {
	args := splitArgs(line)
	if len(args) == 0 {
		return true
	}
	t := &b.trade
	switch name, rest := args[0], args[1:]; name {
	case "help", "?":
		fmt.Println(help)
	case "quit", "exit":
		return false
	case "status":
		b.status()
	case "invite":
		b.request()
	case "accept":
		switch t.phase {
		case phaseInvited:
			b.answer(true)
		case phaseOpen:
			b.accept()
		default:
			say("nothing to accept: the trade is %s", phaseNames[t.phase])
		}
	case "decline":
		switch t.phase {
		case phaseInvited:
			b.answer(false)
		case phaseInviting:
			b.cancel(pb.TradeCloseReason_TRADE_CLOSE_REASON_CANCELLED)
		default:
			say("nothing to decline: the trade is %s", phaseNames[t.phase])
		}
	case "cancel":
		reason := pb.TradeCloseReason_TRADE_CLOSE_REASON_CANCELLED
		if len(rest) > 0 {
			value, ok := pb.TradeCloseReason_value["TRADE_CLOSE_REASON_"+strings.ToUpper(rest[0])]
			if !ok {
				say("unknown reason %q", rest[0])
				return true
			}
			reason = pb.TradeCloseReason(value)
		}
		if t.phase != phaseInvited && t.phase != phaseInviting && t.phase != phaseOpen {
			say("nothing to cancel: the trade is %s", phaseNames[t.phase])
			return true
		}
		b.cancel(reason)
	case "auto":
		on, ok := parseSwitch(rest, 1)
		switch {
		case !ok:
			say("usage: auto answer|accept on|off")
		case rest[0] == "answer":
			b.autoAnswer = on
			say("auto answer %s", onOff(on))
		case rest[0] == "accept":
			b.autoAccept = on
			say("auto accept %s", onOff(on))
		default:
			say("usage: auto answer|accept on|off")
		}
	case "add":
		item, err := parseItem(rest)
		if err != nil {
			say("%v", err)
			return true
		}
		b.editOffer(append(b.offer, item))
	case "count", "remove":
		index, ok := b.lineArg(rest)
		if !ok {
			return true
		}
		items := append(b.offer[:0:0], b.offer...)
		if name == "remove" {
			items = append(items[:index], items[index+1:]...)
		} else {
			count, err := strconv.ParseUint(argAt(rest, 1), 10, 32)
			if err != nil {
				say("usage: count <line> <count>")
				return true
			}
			edited := proto.CloneOf(items[index])
			edited.Count = uint32(count)
			items[index] = edited
		}
		b.editOffer(items)
	case "clear":
		b.editOffer(nil)
	case "kit":
		b.editOffer(append(b.offer, testKit()...))
	case "copy":
		if t.phase != phaseOpen {
			say("no trade is open")
			return true
		}
		var lines []int
		for _, arg := range rest {
			n, err := strconv.Atoi(arg)
			if err != nil || n < 1 || n > len(t.peerItems) {
				say("line %q is not 1..%d", arg, len(t.peerItems))
				return true
			}
			lines = append(lines, n-1)
		}
		b.copyPeer(lines)
	case "mirror":
		on, ok := parseSwitch(rest, 0)
		if !ok {
			say("usage: mirror on|off")
			return true
		}
		b.mirror = on
		say("mirror %s", onOff(on))
		if on && t.phase == phaseOpen {
			b.copyPeer(nil)
		}
	case "skip":
		b.skipCommand(rest)
	case "follow":
		b.mode = modeFollow
		say("following %s", b.target.name)
	case "stay":
		b.mode = modeStay
		say("standing still")
	case "leave":
		b.mode = modeLeave
		say("running away from %s", b.target.name)
	case "target":
		if len(rest) != 1 {
			say("usage: target <name>")
			return true
		}
		b.chooseTarget(rest[0])
		say("looking for %s", rest[0])
	default:
		say("unknown command %q; type help", name)
	}
	return true
}

func (b *bot) editOffer(items []*pb.TradeItem) {
	if err := checkOffer(items); err != nil {
		say("the server would refuse that offer: %v", err)
		return
	}
	b.offer = items
	printItems(b.offer, nil, "  ")
	b.offerChanged()
}

func (b *bot) lineArg(args []string) (int, bool) {
	n, err := strconv.Atoi(argAt(args, 0))
	if err != nil || n < 1 || n > len(b.offer) {
		say("the line must be 1..%d", len(b.offer))
		return 0, false
	}
	return n - 1, true
}

func (b *bot) skipCommand(args []string) {
	switch argAt(args, 0) {
	case "":
		if len(b.skips) == 0 {
			say("every line is reported as receivable")
		}
		for _, rule := range b.skips {
			say("  %s", rule)
		}
		return
	case "clear":
		b.skips = nil
	case "plugin":
		if len(args) != 2 {
			say("usage: skip plugin <name>")
			return
		}
		b.skips = append(b.skips, skipRule{plugin: args[1]})
	case "form":
		if len(args) != 3 {
			say("usage: skip form <plugin>:<id> missing|wrong|effect|enchantment")
			return
		}
		form, err := parseForm(args[1])
		if err == nil {
			var reason pb.TradeSkipReason
			if reason, err = parseSkipReason(args[2]); err == nil {
				b.skips = append(b.skips, skipRule{form: form, reason: reason})
			}
		}
		if err != nil {
			say("%v", err)
			return
		}
	default:
		say("usage: skip [plugin <name> | form <form> <reason> | clear]")
		return
	}
	say("the rules apply from the target's next offer (the server takes one report per revision)")
}

func (b *bot) status() {
	t := &b.target
	switch {
	case t.slot == 0:
		say("target: none yet")
	case !t.located:
		say("target: %s (slot %d), not in the world", t.name, t.slot)
	default:
		say("target: %s (slot %d), %.0f units away; mode %s", t.name, t.slot, b.distanceToTarget(),
			[...]string{"follow", "stay", "leave"}[b.mode])
	}
	say("auto answer %s, auto accept %s, mirror %s", onOff(b.autoAnswer), onOff(b.autoAccept), onOff(b.mirror))
	tr := &b.trade
	if tr.phase == phaseIdle {
		say("no trade; the bot's offer:")
		printItems(b.offer, nil, "  ")
		return
	}
	say("trade %d with %s: %s", tr.id, tr.peer, phaseNames[tr.phase])
	if tr.phase != phaseOpen {
		return
	}
	report := "awaiting the report"
	if tr.reported {
		report = "reported"
	}
	say("the bot's revision %d (%s, accepted %s):", tr.ownRevision, report, yesNo(tr.ownAccepted))
	printItems(b.offer, tr.peerSkips, "  ")
	say("%s's revision %d (accepted %s):", tr.peer, tr.peerRevision, yesNo(tr.peerAccepted))
	printItems(tr.peerItems, tr.ownSkips, "  ")
}

// splitArgs splits on spaces outside double quotes and drops the quotes; a backslash keeps the
// next character.
func splitArgs(line string) []string {
	var args []string
	var current strings.Builder
	quoted, started, escaped := false, false, false
	for _, r := range line {
		switch {
		case escaped:
			current.WriteRune(r)
			escaped = false
		case r == '\\':
			escaped, started = true, true
		case r == '"':
			quoted, started = !quoted, true
		case r == ' ' && !quoted:
			if started {
				args = append(args, current.String())
				current.Reset()
				started = false
			}
		default:
			current.WriteRune(r)
			started = true
		}
	}
	if started {
		args = append(args, current.String())
	}
	return args
}

func argAt(args []string, i int) string {
	if i < len(args) {
		return args[i]
	}
	return ""
}

func parseSwitch(args []string, i int) (bool, bool) {
	switch argAt(args, i) {
	case "on":
		return true, len(args) == i+1
	case "off":
		return false, len(args) == i+1
	}
	return false, false
}

func onOff(on bool) string {
	if on {
		return "on"
	}
	return "off"
}

func yesNo(v bool) string {
	if v {
		return "yes"
	}
	return "no"
}
