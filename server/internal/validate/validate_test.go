package validate

import (
	"math"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/wire/pb"
)

func ref(plugin string, formID uint32) *pb.FormRef {
	return &pb.FormRef{Plugin: plugin, FormId: formID}
}

func TestCheckpoint(t *testing.T) {
	valid := &pb.Checkpoint{Sequence: 1, X: -100, Heading: 4095, Pitch: -1609, Speed: 100000, Direction: 255,
		Flags: 6 | uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SWIMMING)}
	cases := map[string]func(*pb.Checkpoint){
		"heading above 4095":  func(c *pb.Checkpoint) { c.Heading = 4096 },
		"pitch above π/2":     func(c *pb.Checkpoint) { c.Pitch = 1610 },
		"pitch below -π/2":    func(c *pb.Checkpoint) { c.Pitch = -1610 },
		"speed above bound":   func(c *pb.Checkpoint) { c.Speed = 100001 },
		"direction above 255": func(c *pb.Checkpoint) { c.Direction = 256 },
		"unknown movement":    func(c *pb.Checkpoint) { c.Flags = 7 },
		"unknown flag bit":    func(c *pb.Checkpoint) { c.Flags = 1 << 12 },
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
	if err := Context(&pb.Context{CellId: 0xFFFFFFFF, GraphEpoch: 1}); err != nil {
		t.Fatal(err)
	}
	if err := Context(&pb.Context{GraphEpoch: 1, Furniture: ref("Skyrim.esm", 0xFFFFFF)}); err != nil {
		t.Fatal(err)
	}
	for name, c := range map[string]*pb.Context{
		"zero graph epoch":      {},
		"empty furniture":       {GraphEpoch: 1, Furniture: ref("", 1)},
		"long plugin":           {GraphEpoch: 1, Furniture: ref(strings.Repeat("a", 261), 1)},
		"form ID above 24 bits": {GraphEpoch: 1, Furniture: ref("Skyrim.esm", 0x1000000)},
	} {
		if Context(c) == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestAction(t *testing.T) {
	for name, a := range map[string]*pb.Action{
		"normal":       {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: pb.AttackAction_ATTACK_ACTION_NORMAL},
		"bash":         {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: pb.AttackAction_ATTACK_ACTION_BASH},
		"power right":  {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: pb.AttackAction_ATTACK_ACTION_DUAL, Power: true, Direction: pb.PowerDirection_POWER_DIRECTION_RIGHT},
		"power stands": {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: pb.AttackAction_ATTACK_ACTION_LEFT, Power: true, Direction: pb.PowerDirection_POWER_DIRECTION_STANDING},
	} {
		if err := Action(a); err != nil {
			t.Errorf("%s rejected", name)
		}
	}
	for name, a := range map[string]*pb.Action{
		"no action":            {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1},
		"unknown action":       {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: 5},
		"zero action sequence": {GraphEpoch: 1, CheckpointSequence: 1, Action: 1},
		"zero graph epoch":     {ActionSequence: 1, CheckpointSequence: 1, Action: 1},
		"zero checkpoint":      {ActionSequence: 1, GraphEpoch: 1, Action: 1},
		"power without dir":    {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: 1, Power: true},
		"dir without power":    {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: 1, Direction: pb.PowerDirection_POWER_DIRECTION_LEFT},
		"unknown direction":    {ActionSequence: 1, GraphEpoch: 1, CheckpointSequence: 1, Action: 1, Power: true, Direction: 6},
	} {
		if Action(a) == nil {
			t.Errorf("%s accepted", name)
		}
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
		FaceParts:   []int32{0, -1, math.MaxInt32, math.MinInt32},
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
		"spell with form":        func(e *pb.Equipment) { e.LeftHand.Form = ref("Skyrim.esm", 1) },
		"weapon without type":    func(e *pb.Equipment) { e.RightHand.WeaponType = 0 },
		"weapon type 10":         func(e *pb.Equipment) { e.RightHand.WeaponType = 10 },
		"left two-handed":        func(e *pb.Equipment) { e.LeftHand = weapon(6) },
		"two-handed with left":   func(e *pb.Equipment) { e.RightHand = weapon(9) },
		"unspecified hand":       func(e *pb.Equipment) { e.LeftHand = &pb.Hand{} },
		"modded outfit armor":    func(e *pb.Equipment) { e.DefaultOutfit.Armor[0].Form = ref("Mod.esp", 1) },
		"too much outfit armor":  func(e *pb.Equipment) { e.DefaultOutfit.Armor = make([]*pb.Armor, 9) },
		"duplicate outfit hand":  func(e *pb.Equipment) { e.DefaultOutfit.Hands[1] = weapon(5) },
		"outfit spell":           func(e *pb.Equipment) { e.DefaultOutfit.Hands[1] = &pb.Hand{Kind: pb.HandKind_HAND_KIND_SPELL} },
		"modded outfit ammo":     func(e *pb.Equipment) { e.DefaultOutfit.Ammo = ref("Mod.esp", 1) },
		"invalid ammo reference": func(e *pb.Equipment) { e.Ammo = ref("", 1) },
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
	if text, err := Chat(send("  hello there ", pb.ChatChannel_CHAT_CHANNEL_LOCAL)); err != nil || text != "hello there" {
		t.Fatalf("got %q, %v", text, err)
	}
	if _, err := Chat(send(strings.Repeat("é", 256), pb.ChatChannel_CHAT_CHANNEL_WHISPER)); err != nil {
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
}
