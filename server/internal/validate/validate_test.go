package validate

import (
	"math"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire/pb"
)

func ref(plugin string, formID uint32) *pb.FormRef {
	return &pb.FormRef{Plugin: plugin, FormId: formID}
}

func TestCheckpoint(t *testing.T) {
	valid := &pb.Checkpoint{Sequence: 1, X: -100, Heading: 4095, Pitch: -1609, Speed: 100000, Direction: 255,
		Flags: 6 | uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SWIMMING|pb.CheckpointFlag_CHECKPOINT_FLAG_GROUNDED), Time: 16383}
	cases := map[string]func(*pb.Checkpoint){
		"heading above 4095":  func(c *pb.Checkpoint) { c.Heading = 4096 },
		"pitch above π/2":     func(c *pb.Checkpoint) { c.Pitch = 1610 },
		"pitch below -π/2":    func(c *pb.Checkpoint) { c.Pitch = -1610 },
		"speed above bound":   func(c *pb.Checkpoint) { c.Speed = 100001 },
		"direction above 255": func(c *pb.Checkpoint) { c.Direction = 256 },
		"time above 16383":    func(c *pb.Checkpoint) { c.Time = 16384 },
		"unknown movement":    func(c *pb.Checkpoint) { c.Flags = 7 },
		"unknown flag bit":    func(c *pb.Checkpoint) { c.Flags = 1 << 16 },
	}
	if err := Checkpoint(valid); err != nil {
		t.Fatalf("valid checkpoint rejected: %v", err)
	}
	for name, mutate := range cases {
		c := proto.Clone(valid).(*pb.Checkpoint)
		mutate(c)
		if Checkpoint(c) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestContext(t *testing.T) {
	if err := Context(&pb.Context{InteriorCellId: 0xFFFFFFFF, GraphEpoch: 1}); err != nil {
		t.Fatal(err)
	}
	if err := Context(&pb.Context{WorldspaceId: 0x3C, GraphEpoch: 1}); err != nil {
		t.Fatal(err)
	}
	if err := Context(&pb.Context{GraphEpoch: 1, Furniture: ref("Skyrim.esm", 0xFFFFFF)}); err != nil {
		t.Fatal(err)
	}
	if err := Context(&pb.Context{GraphEpoch: 1, Shapeshift: pb.Shapeshift_SHAPESHIFT_WEREBEAR}); err != nil {
		t.Fatal(err)
	}
	for name, c := range map[string]*pb.Context{
		"zero graph epoch":      {},
		"cell in a worldspace":  {InteriorCellId: 5, WorldspaceId: 0x3C, GraphEpoch: 1},
		"empty furniture":       {GraphEpoch: 1, Furniture: ref("", 1)},
		"long plugin":           {GraphEpoch: 1, Furniture: ref(strings.Repeat("a", 261), 1)},
		"form ID above 24 bits": {GraphEpoch: 1, Furniture: ref("Skyrim.esm", 0x1000000)},
		"unknown shapeshift":    {GraphEpoch: 1, Shapeshift: 4},
		"negative shapeshift":   {GraphEpoch: 1, Shapeshift: -1},
	} {
		if Context(c) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestAction(t *testing.T) {
	attack := func(action pb.AttackAction, direction pb.PowerDirection) *pb.Action {
		return &pb.Action{ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1,
			Kind: &pb.Action_Attack{Attack: &pb.Attack{Action: action, Direction: direction}}}
	}
	beast := func(action pb.BeastAction) *pb.Action {
		return &pb.Action{ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Kind: &pb.Action_Beast{Beast: action}}
	}
	for name, a := range map[string]*pb.Action{
		"normal":       attack(pb.AttackAction_ATTACK_ACTION_NORMAL, pb.PowerDirection_POWER_DIRECTION_NONE),
		"bash":         attack(pb.AttackAction_ATTACK_ACTION_BASH, pb.PowerDirection_POWER_DIRECTION_NONE),
		"power right":  attack(pb.AttackAction_ATTACK_ACTION_DUAL, pb.PowerDirection_POWER_DIRECTION_RIGHT),
		"power stands": attack(pb.AttackAction_ATTACK_ACTION_LEFT, pb.PowerDirection_POWER_DIRECTION_STANDING),
		"howl":         beast(pb.BeastAction_BEAST_ACTION_WEREWOLF_HOWL_START),
		"last beast":   beast(pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_CAST_INTERRUPT_RIGHT),
	} {
		if err := Action(a); err != nil {
			t.Errorf("%s rejected", name)
		}
	}
	zeroAction, zeroEpoch, zeroCheckpoint := attack(1, 0), attack(1, 0), attack(1, 0)
	zeroAction.ActionSequence, zeroEpoch.GraphEpoch, zeroCheckpoint.CheckpointSequence = 0, 0, 0
	for name, a := range map[string]*pb.Action{
		"no kind":              {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1},
		"empty attack":         {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Kind: &pb.Action_Attack{}},
		"unspecified attack":   attack(pb.AttackAction_ATTACK_ACTION_UNSPECIFIED, pb.PowerDirection_POWER_DIRECTION_NONE),
		"unknown action":       attack(5, pb.PowerDirection_POWER_DIRECTION_NONE),
		"zero action sequence": zeroAction,
		"zero graph epoch":     zeroEpoch,
		"zero checkpoint":      zeroCheckpoint,
		"unknown direction":    attack(1, 6),
		"negative direction":   attack(1, -1),
		"unspecified beast":    beast(pb.BeastAction_BEAST_ACTION_UNSPECIFIED),
		"beast gap value":      beast(20),
		"beast past the last":  beast(52),
	} {
		if Action(a) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestBeastActionForm(t *testing.T) {
	for _, c := range []struct {
		action   pb.BeastAction
		form     pb.Shapeshift
		werebear bool
	}{
		{pb.BeastAction_BEAST_ACTION_WEREWOLF_ATTACK_RIGHT, pb.Shapeshift_SHAPESHIFT_WEREWOLF, true},
		{pb.BeastAction_BEAST_ACTION_WEREWOLF_FEED_PAIRED, pb.Shapeshift_SHAPESHIFT_WEREWOLF, true},
		{pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_ATTACK_RIGHT, pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD, false},
		{pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_CAST_INTERRUPT_RIGHT, pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD, false},
		{pb.BeastAction_BEAST_ACTION_UNSPECIFIED, pb.Shapeshift_SHAPESHIFT_NONE, false},
		{20, pb.Shapeshift_SHAPESHIFT_NONE, false},
	} {
		if got := BeastActionForm(c.action); got != c.form {
			t.Errorf("%v: form %v, want %v", c.action, got, c.form)
		}
		if got := SameBeastGraph(pb.Shapeshift_SHAPESHIFT_WEREBEAR, BeastActionForm(c.action)); got != c.werebear {
			t.Errorf("%v: werebear plays it %v, want %v", c.action, got, c.werebear)
		}
	}
	if SameBeastGraph(pb.Shapeshift_SHAPESHIFT_NONE, pb.Shapeshift_SHAPESHIFT_NONE) {
		t.Error("human form plays beast actions")
	}
}

func validAppearance() *pb.Appearance {
	return &pb.Appearance{
		Revision:    3,
		Name:        "Lydia Stormblade",
		Level:       12,
		Race:        ref("Skyrim.esm", 79686),
		Weight:      50,
		Height:      1,
		HeadParts:   []*pb.HeadPart{{Form: ref("Skyrim.esm", 324286), Type: 1}},
		HairColor:   &pb.Color{R: 59, G: 42, B: 30},
		BodyTint:    &pb.Color{R: 222, G: 188, B: 170},
		FaceMorphs:  make([]float32, FaceMorphCount),
		FaceParts:   []int32{0, -1, 255, 3},
		Tints:       []*pb.Tint{{Index: 44, Type: 6, Color: &pb.Color{R: 1}, Alpha: 1}},
		DefaultRace: &pb.DefaultRace{Race: ref("skyrim.ESM", 1), Female: true},
	}
}

func TestAppearance(t *testing.T) {
	valid := validAppearance()
	valid.FaceMorphs[0] = math.MaxFloat32
	if err := Appearance(valid); err != nil {
		t.Fatal(err)
	}
	cases := map[string]func(*pb.Appearance){
		"zero revision":        func(a *pb.Appearance) { a.Revision = 0 },
		"long name":            func(a *pb.Appearance) { a.Name = strings.Repeat("é", 65) },
		"control in name":      func(a *pb.Appearance) { a.Name = "a\x7f" },
		"level above 65535":    func(a *pb.Appearance) { a.Level = 65536 },
		"missing race":         func(a *pb.Appearance) { a.Race = nil },
		"weight above 100":     func(a *pb.Appearance) { a.Weight = 100.5 },
		"height below 0.25":    func(a *pb.Appearance) { a.Height = 0.2 },
		"NaN height":           func(a *pb.Appearance) { a.Height = float32(math.NaN()) },
		"too many head parts":  func(a *pb.Appearance) { a.HeadParts = make([]*pb.HeadPart, 17) },
		"head part type":       func(a *pb.Appearance) { a.HeadParts[0].Type = 7 },
		"head part no form":    func(a *pb.Appearance) { a.HeadParts[0].Form = nil },
		"hair channel":         func(a *pb.Appearance) { a.HairColor.G = 256 },
		"missing body tint":    func(a *pb.Appearance) { a.BodyTint = nil },
		"18 face morphs":       func(a *pb.Appearance) { a.FaceMorphs = a.FaceMorphs[:18] },
		"infinite face morph":  func(a *pb.Appearance) { a.FaceMorphs[3] = float32(math.Inf(1)) },
		"3 face parts":         func(a *pb.Appearance) { a.FaceParts = a.FaceParts[:3] },
		"face part above 255":  func(a *pb.Appearance) { a.FaceParts[2] = 256 },
		"face part below -1":   func(a *pb.Appearance) { a.FaceParts[1] = math.MinInt32 },
		"too many tints":       func(a *pb.Appearance) { a.Tints = make([]*pb.Tint, 65) },
		"tint type":            func(a *pb.Appearance) { a.Tints[0].Type = 15 },
		"tint alpha":           func(a *pb.Appearance) { a.Tints[0].Alpha = 1.01 },
		"tint without color":   func(a *pb.Appearance) { a.Tints[0].Color = nil },
		"modded default race":  func(a *pb.Appearance) { a.DefaultRace.Race = ref("Mod.esp", 1) },
		"default race no race": func(a *pb.Appearance) { a.DefaultRace.Race = nil },
	}
	for name, mutate := range cases {
		a := validAppearance()
		mutate(a)
		if Appearance(a) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func weapon(weaponType uint32) *pb.Hand {
	return &pb.Hand{Kind: pb.HandKind_HAND_KIND_WEAPON, Form: ref("Skyrim.esm", 77936), WeaponType: weaponType}
}

func validEquipment() *pb.Equipment {
	return &pb.Equipment{
		Revision:  7,
		Armor:     []*pb.Armor{{Form: ref("Skyrim.esm", 77385), SlotMask: 4}, {Form: ref("Mod.esp", 1), SlotMask: 0xFFFFFFFF}},
		RightHand: weapon(1),
		LeftHand:  &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL},
		Ammo:      ref("Skyrim.esm", 118996),
		DefaultOutfit: &pb.DefaultOutfit{
			Armor: []*pb.Armor{{Form: ref("Dawnguard.esm", 1), SlotMask: 1}},
			Hands: []*pb.Hand{weapon(5), {Kind: pb.HandKind_HAND_KIND_TORCH, Form: ref("Skyrim.esm", 2)}},
			Ammo:  ref("Skyrim.esm", 3),
		},
	}
}

func TestEquipment(t *testing.T) {
	if err := Equipment(validEquipment()); err != nil {
		t.Fatal(err)
	}
	accepted := map[string]func(*pb.Equipment){
		"empty hands":      func(e *pb.Equipment) { e.RightHand, e.LeftHand = nil, nil },
		"two-handed alone": func(e *pb.Equipment) { e.RightHand, e.LeftHand = weapon(7), nil },
		"left torch": func(e *pb.Equipment) {
			e.LeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_TORCH, Form: ref("Skyrim.esm", 1)}
		},
		"no default outfit": func(e *pb.Equipment) { e.DefaultOutfit = nil },
		"spell with look": func(e *pb.Equipment) {
			e.LeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL, Form: ref("Mod.esp", 1),
				CastingArt: ref("Skyrim.esm", 2), CastingLight: ref("Skyrim.esm", 3)}
		},
		"right spell": func(e *pb.Equipment) { e.RightHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL} },
		"beast spells": func(e *pb.Equipment) {
			e.BeastRightHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL, Form: ref("Dawnguard.esm", 0x2DE)}
			e.BeastLeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL, CastingArt: ref("Skyrim.esm", 2)}
		},
		"beast armor": func(e *pb.Equipment) {
			e.BeastArmor = []*pb.Armor{{Form: ref("Dawnguard.esm", 0x11A84), SlotMask: 0x40}, {Form: ref("Mod.esp", 1), SlotMask: 1}}
		},
	}
	for name, mutate := range accepted {
		e := validEquipment()
		mutate(e)
		if err := Equipment(e); err != nil {
			t.Errorf("%s rejected", name)
		}
	}
	rejected := map[string]func(*pb.Equipment){
		"zero revision":   func(e *pb.Equipment) { e.Revision = 0 },
		"too much armor":  func(e *pb.Equipment) { e.Armor = make([]*pb.Armor, 33) },
		"zero slot mask":  func(e *pb.Equipment) { e.Armor[0].SlotMask = 0 },
		"duplicate armor": func(e *pb.Equipment) { e.Armor[1].Form = ref("Skyrim.esm", 77385) },
		"right torch": func(e *pb.Equipment) {
			e.RightHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_TORCH, Form: ref("Skyrim.esm", 1)}
		},
		"spell with weapon type":  func(e *pb.Equipment) { e.LeftHand.WeaponType = 1 },
		"invalid spell":           func(e *pb.Equipment) { e.LeftHand.Form = ref("", 1) },
		"invalid casting art":     func(e *pb.Equipment) { e.LeftHand.CastingArt = ref("Skyrim.esm", 0x1000000) },
		"invalid casting light":   func(e *pb.Equipment) { e.LeftHand.CastingLight = ref("", 1) },
		"weapon with casting art": func(e *pb.Equipment) { e.RightHand.CastingArt = ref("Skyrim.esm", 1) },
		"torch with casting light": func(e *pb.Equipment) {
			e.LeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_TORCH, Form: ref("Skyrim.esm", 1),
				CastingLight: ref("Skyrim.esm", 2)}
		},
		"outfit hand with casting art": func(e *pb.Equipment) { e.DefaultOutfit.Hands[0].CastingArt = ref("Skyrim.esm", 1) },
		"weapon without type":          func(e *pb.Equipment) { e.RightHand.WeaponType = 0 },
		"weapon type 10":               func(e *pb.Equipment) { e.RightHand.WeaponType = 10 },
		"left two-handed":              func(e *pb.Equipment) { e.LeftHand = weapon(6) },
		"two-handed with left":         func(e *pb.Equipment) { e.RightHand = weapon(9) },
		"unspecified hand":             func(e *pb.Equipment) { e.LeftHand = &pb.Hand{} },
		"modded outfit armor":          func(e *pb.Equipment) { e.DefaultOutfit.Armor[0].Form = ref("Mod.esp", 1) },
		"creation club outfit armor": func(e *pb.Equipment) {
			e.DefaultOutfit.Armor[0].Form = ref("ccBGSSSE052-BA_Iron.esl", 0x801)
		},
		"too much outfit armor":  func(e *pb.Equipment) { e.DefaultOutfit.Armor = make([]*pb.Armor, 9) },
		"duplicate outfit hand":  func(e *pb.Equipment) { e.DefaultOutfit.Hands[1] = weapon(5) },
		"outfit spell":           func(e *pb.Equipment) { e.DefaultOutfit.Hands[1] = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL} },
		"modded outfit ammo":     func(e *pb.Equipment) { e.DefaultOutfit.Ammo = ref("Mod.esp", 1) },
		"invalid ammo reference": func(e *pb.Equipment) { e.Ammo = ref("", 1) },
		"beast weapon":           func(e *pb.Equipment) { e.BeastRightHand = weapon(1) },
		"beast torch": func(e *pb.Equipment) {
			e.BeastLeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_TORCH, Form: ref("Skyrim.esm", 1)}
		},
		"invalid beast spell": func(e *pb.Equipment) {
			e.BeastLeftHand = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL, Form: ref("", 1)}
		},
		"beast armor without slots": func(e *pb.Equipment) {
			e.BeastArmor = []*pb.Armor{{Form: ref("Dawnguard.esm", 0x11A84)}}
		},
		"duplicate beast armor": func(e *pb.Equipment) {
			piece := &pb.Armor{Form: ref("Dawnguard.esm", 0x11A84), SlotMask: 0x40}
			e.BeastArmor = []*pb.Armor{piece, piece}
		},
		"too much beast armor": func(e *pb.Equipment) {
			for i := range MaximumBeastArmor + 1 {
				e.BeastArmor = append(e.BeastArmor, &pb.Armor{Form: ref("Mod.esp", uint32(i+1)), SlotMask: 1})
			}
		},
	}
	for name, mutate := range rejected {
		e := validEquipment()
		mutate(e)
		if Equipment(e) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestChat(t *testing.T) {
	send := func(text string, channel pb.ChatChannel) *pb.ChatSend {
		return &pb.ChatSend{Text: text, Channel: channel}
	}
	if text, err := Chat(send("  hello there ", pb.ChatChannel_CHAT_CHANNEL_NEARBY)); err != nil || text != "hello there" {
		t.Fatalf("got %q, %v", text, err)
	}
	if _, err := Chat(send(strings.Repeat("é", 256), pb.ChatChannel_CHAT_CHANNEL_GLOBAL)); err != nil {
		t.Fatal("text at the byte limit rejected")
	}
	for _, text := range []string{"", "   ", "tab\there", "bell\x07", strings.Repeat("a", 513), "\xff"} {
		if _, err := Chat(send(text, pb.ChatChannel_CHAT_CHANNEL_GLOBAL)); err == nil {
			t.Errorf("%q accepted", text)
		}
	}
	if _, err := Chat(send("hi", pb.ChatChannel(3))); err == nil {
		t.Error("unknown channel accepted")
	}
	whisper := send("hi", pb.ChatChannel_CHAT_CHANNEL_WHISPER)
	if _, err := Chat(whisper); err == nil {
		t.Error("whisper without a recipient accepted")
	}
	whisper.Recipient = 7
	if text, err := Chat(whisper); err != nil || text != "hi" {
		t.Errorf("whisper: got %q, %v", text, err)
	}
	for _, channel := range []pb.ChatChannel{pb.ChatChannel_CHAT_CHANNEL_GLOBAL, pb.ChatChannel_CHAT_CHANNEL_NEARBY} {
		line := send("hi", channel)
		line.Recipient = 7
		if _, err := Chat(line); err == nil {
			t.Errorf("%v with a recipient accepted", channel)
		}
	}
}

func TestHello(t *testing.T) {
	token := make([]byte, SessionTokenBytes)
	for name, h := range map[string]*pb.Hello{
		"a new session":     {DisplayName: "Lydia"},
		"a resumed session": {DisplayName: "Lydia", SessionToken: token},
		"every class":       {DisplayName: "Ulfric_Storm_99"},
		"at the limit":      {DisplayName: strings.Repeat("a", MaximumDisplayNameBytes)},
		"at the minimum":    {DisplayName: "_a1"},
		"vanished players":  {DisplayName: "Lydia", Vanished: []uint64{2, 3}},
		"vanished at limit": {DisplayName: "Lydia", Vanished: sessions(MaximumVanished)},
	} {
		if err := Hello(h); err != nil {
			t.Errorf("%s rejected: %v", name, err)
		}
	}
	for name, h := range map[string]*pb.Hello{
		"no display name": {SessionToken: token},
		"a space":         {DisplayName: "Ulfric Stormclk"},
		"only a space":    {DisplayName: " "},
		"under the limit": {DisplayName: "Ly"},
		"punctuation":     {DisplayName: "Lydia-2"},
		"non-Latin":       {DisplayName: "Lydiä"},
		"control":         {DisplayName: "Ly\tdia"},
		"over the limit":  {DisplayName: strings.Repeat("a", MaximumDisplayNameBytes+1)},
		"invalid UTF-8":   {DisplayName: "\xff"},
		"short token":     {DisplayName: "Lydia", SessionToken: token[:31]},
		"long token":      {DisplayName: "Lydia", SessionToken: make([]byte, 33)},
		"vanished zero":   {DisplayName: "Lydia", Vanished: []uint64{2, 0}},
		"vanished many":   {DisplayName: "Lydia", Vanished: sessions(MaximumVanished + 1)},
	} {
		if Hello(h) == nil {
			t.Errorf("%s accepted", name)
		}
	}
	if DisplayNameKey("LyDia") != DisplayNameKey("lydia") || DisplayNameKey("Lydia") == DisplayNameKey("Lydia2") {
		t.Error("display name keys do not compare without case")
	}
}

func sessions(count int) []uint64 {
	result := make([]uint64, count)
	for i := range result {
		result[i] = uint64(i + 1)
	}
	return result
}

func TestVanish(t *testing.T) {
	vanish := func(session uint64) *pb.ClientMessage {
		return &pb.ClientMessage{Message: &pb.ClientMessage_Vanish{Vanish: &pb.Vanish{SessionId: session, Vanished: true}}}
	}
	if err := Message(vanish(7)); err != nil {
		t.Errorf("vanish rejected: %v", err)
	}
	if Message(vanish(0)) != ErrVanish {
		t.Error("vanish without a session accepted")
	}
}

func TestInspect(t *testing.T) {
	inspect := func(slot uint32) *pb.ClientMessage {
		return &pb.ClientMessage{Message: &pb.ClientMessage_Inspect{Inspect: &pb.Inspect{Slot: slot}}}
	}
	if err := Message(inspect(3)); err != nil {
		t.Errorf("inspect rejected: %v", err)
	}
	if Message(inspect(0)) != ErrInspect {
		t.Error("inspect without a slot accepted")
	}
}
