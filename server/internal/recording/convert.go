package recording

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"

	"google.golang.org/protobuf/encoding/protojson"

	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

func readVersionedHeader(lines [][]byte, version int) (Header, error) {
	if len(lines) == 0 {
		return Header{}, errorf("empty recording")
	}
	var header headerLine
	if err := json.Unmarshal(lines[0], &header); err != nil || header.Type != "header" || header.Format != FormatName {
		return Header{}, errorf("not a %s file", FormatName)
	}
	if header.Version != version {
		return Header{}, errorf("recording format version %d, expected %d", header.Version, version)
	}
	return header.Header, nil
}

// ConvertV3 scales version 3's whole-unit checkpoint positions to quarter units.
func ConvertV3(data []byte) ([]byte, error) {
	lines := splitLines(data)
	header, err := readVersionedHeader(lines, 3)
	if err != nil {
		return nil, err
	}

	var out bytes.Buffer
	out.Write(EncodeHeader(header))
	out.WriteByte('\n')
	for i, line := range lines[1:] {
		number := i + 2
		var body bodyLine
		if err := json.Unmarshal(line, &body); err != nil {
			if number == len(lines) {
				break
			}
			return nil, errorf("line %d: invalid JSON: %v", number, err)
		}
		if body.Type != "message" || body.T == nil {
			out.Write(line)
			out.WriteByte('\n')
			continue
		}
		message := &pb.ClientMessage{}
		if err := protojson.Unmarshal(body.Message, message); err != nil {
			return nil, errorf("line %d: invalid message: %v", number, err)
		}
		c := message.GetCheckpoint()
		if c == nil {
			out.Write(line)
			out.WriteByte('\n')
			continue
		}
		c.X, c.Y, c.Z = scalePosition(c.X), scalePosition(c.Y), scalePosition(c.Z)
		encoded, err := EncodeMessage(Message{*body.T, message})
		if err != nil {
			return nil, err
		}
		out.Write(encoded)
		out.WriteByte('\n')
	}
	if _, err := Parse(out.Bytes()); err != nil {
		return nil, fmt.Errorf("converted recording is invalid: %w", err)
	}
	return out.Bytes(), nil
}

func scalePosition(v int32) int32 {
	return int32(max(math.MinInt32, min(math.MaxInt32, int64(v)*wire.PositionScale)))
}

func ConvertV2(data []byte) ([]byte, error) {
	lines := splitLines(data)
	header, err := readVersionedHeader(lines, 2)
	if err != nil {
		return nil, err
	}

	var out bytes.Buffer
	out.Write(EncodeHeader(header))
	out.WriteByte('\n')
	var context *pb.Context
	for i, line := range lines[1:] {
		number := i + 2
		var raw struct {
			Type    string          `json:"type"`
			T       int64           `json:"t"`
			Event   string          `json:"event"`
			Payload json.RawMessage `json:"payload"`
			Reason  string          `json:"reason"`
		}
		if err := json.Unmarshal(line, &raw); err != nil {
			if number == len(lines) {
				break
			}
			return nil, errorf("line %d: invalid JSON: %v", number, err)
		}
		if raw.Type == "end" {
			out.Write(EncodeEnd(End{raw.T, raw.Reason}))
			out.WriteByte('\n')
			continue
		}
		messages, err := convertV2Message(raw.Event, raw.Payload, &context)
		if err != nil {
			return nil, errorf("line %d: %v", number, err)
		}
		for _, message := range messages {
			encoded, err := EncodeMessage(Message{raw.T, message})
			if err != nil {
				return nil, err
			}
			out.Write(encoded)
			out.WriteByte('\n')
		}
	}
	if _, err := Parse(out.Bytes()); err != nil {
		return nil, fmt.Errorf("converted recording is invalid: %w", err)
	}
	return out.Bytes(), nil
}

type v2FormRef struct {
	Plugin string `json:"plugin"`
	FormID uint32 `json:"formId"`
}

func (f *v2FormRef) proto() *pb.FormRef {
	if f == nil {
		return nil
	}
	return &pb.FormRef{Plugin: f.Plugin, FormId: f.FormID}
}

type v2Color struct {
	R, G, B uint32
}

func (c *v2Color) proto() *pb.Color {
	if c == nil {
		return nil
	}
	return &pb.Color{R: c.R, G: c.G, B: c.B}
}

var v2MovementStates = map[string]pb.MovementState{
	"idle":            pb.MovementState_MOVEMENT_STATE_IDLE,
	"walking":         pb.MovementState_MOVEMENT_STATE_WALKING,
	"running":         pb.MovementState_MOVEMENT_STATE_RUNNING,
	"sprinting":       pb.MovementState_MOVEMENT_STATE_SPRINTING,
	"sneakingIdle":    pb.MovementState_MOVEMENT_STATE_SNEAKING_IDLE,
	"sneakingWalking": pb.MovementState_MOVEMENT_STATE_SNEAKING_WALKING,
	"sneakingRunning": pb.MovementState_MOVEMENT_STATE_SNEAKING_RUNNING,
}

func convertV2Message(event string, payload json.RawMessage, context **pb.Context) ([]*pb.ClientMessage, error) {
	switch event {
	case "player:enter":
		*context = nil
		return []*pb.ClientMessage{{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}}}, nil
	case "player:exit":
		*context = nil
		return []*pb.ClientMessage{{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}}}, nil
	case "player:update":
		return convertV2Checkpoint(payload, context)
	case "animation:action":
		return convertV2Action(payload)
	case "player:appearance":
		return convertV2Appearance(payload)
	case "player:equipment":
		return convertV2Equipment(payload)
	}
	return nil, fmt.Errorf("unsupported event %q", event)
}

func convertV2Checkpoint(payload json.RawMessage, context **pb.Context) ([]*pb.ClientMessage, error) {
	var u struct {
		X, Y, Z, Heading, Pitch, Speed, Direction                 float64
		MovementState                                             string
		Jumping, WeaponDrawn, BowDrawn, CastingLeft, CastingRight bool
		Blocking, Dead, Mounted, Swimming                         bool
		CellID                                                    uint32 `json:"cellId"`
		WorldspaceID                                              uint32 `json:"worldspaceId"`
		Furniture                                                 *v2FormRef
		GraphEpoch                                                uint64
		Sequence                                                  uint64
	}
	if err := json.Unmarshal(payload, &u); err != nil {
		return nil, err
	}
	state, ok := v2MovementStates[u.MovementState]
	if !ok {
		return nil, fmt.Errorf("unknown movement state %q", u.MovementState)
	}
	var flags pb.CheckpointFlag
	for flag, set := range map[pb.CheckpointFlag]bool{
		pb.CheckpointFlag_CHECKPOINT_FLAG_JUMPING:       u.Jumping,
		pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN:  u.WeaponDrawn,
		pb.CheckpointFlag_CHECKPOINT_FLAG_BOW_DRAWN:     u.BowDrawn,
		pb.CheckpointFlag_CHECKPOINT_FLAG_CASTING_LEFT:  u.CastingLeft,
		pb.CheckpointFlag_CHECKPOINT_FLAG_CASTING_RIGHT: u.CastingRight,
		pb.CheckpointFlag_CHECKPOINT_FLAG_BLOCKING:      u.Blocking,
		pb.CheckpointFlag_CHECKPOINT_FLAG_DEAD:          u.Dead,
		pb.CheckpointFlag_CHECKPOINT_FLAG_MOUNTED:       u.Mounted,
		pb.CheckpointFlag_CHECKPOINT_FLAG_SWIMMING:      u.Swimming,
	} {
		if set {
			flags |= flag
		}
	}
	var messages []*pb.ClientMessage
	next := &pb.Context{CellId: u.CellID, WorldspaceId: u.WorldspaceID, GraphEpoch: u.GraphEpoch, Furniture: u.Furniture.proto()}
	if previous := *context; previous == nil || !sameV2Context(previous, next) {
		*context = next
		messages = append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: next}})
	}
	checkpoint := wire.Quantize(wire.Sample{X: u.X, Y: u.Y, Z: u.Z, Heading: u.Heading, Pitch: u.Pitch, Speed: u.Speed,
		Direction: u.Direction, MovementState: state, Flags: flags}, u.Sequence)
	return append(messages, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: checkpoint}}), nil
}

func sameV2Context(a, b *pb.Context) bool {
	if a.CellId != b.CellId || a.WorldspaceId != b.WorldspaceId || a.GraphEpoch != b.GraphEpoch ||
		(a.Furniture == nil) != (b.Furniture == nil) {
		return false
	}
	return a.Furniture == nil || (a.Furniture.Plugin == b.Furniture.Plugin && a.Furniture.FormId == b.Furniture.FormId)
}

var (
	v2Actions = map[string]pb.AttackAction{
		"normal": pb.AttackAction_ATTACK_ACTION_NORMAL, "left": pb.AttackAction_ATTACK_ACTION_LEFT,
		"dual": pb.AttackAction_ATTACK_ACTION_DUAL, "bash": pb.AttackAction_ATTACK_ACTION_BASH,
	}
	v2Directions = map[string]pb.PowerDirection{
		"standing": pb.PowerDirection_POWER_DIRECTION_STANDING, "forward": pb.PowerDirection_POWER_DIRECTION_FORWARD,
		"backward": pb.PowerDirection_POWER_DIRECTION_BACKWARD, "left": pb.PowerDirection_POWER_DIRECTION_LEFT,
		"right": pb.PowerDirection_POWER_DIRECTION_RIGHT,
	}
)

func convertV2Action(payload json.RawMessage) ([]*pb.ClientMessage, error) {
	var a struct {
		ActionSequence, GraphEpoch, CheckpointSequence uint64
		Action, Direction                              string
		Power                                          bool
	}
	if err := json.Unmarshal(payload, &a); err != nil {
		return nil, err
	}
	action, ok := v2Actions[a.Action]
	if !ok {
		return nil, fmt.Errorf("unknown action %q", a.Action)
	}
	message := &pb.Action{ActionSequence: a.ActionSequence, GraphEpoch: a.GraphEpoch,
		CheckpointSequence: a.CheckpointSequence, Action: action, Power: a.Power}
	if a.Power {
		if message.Direction, ok = v2Directions[a.Direction]; !ok {
			return nil, fmt.Errorf("unknown power direction %q", a.Direction)
		}
	}
	return []*pb.ClientMessage{{Message: &pb.ClientMessage_Action{Action: message}}}, nil
}

func convertV2Appearance(payload json.RawMessage) ([]*pb.ClientMessage, error) {
	var a struct {
		Revision  uint64
		Name      string
		Level     uint32
		Race      *v2FormRef
		Female    bool
		Weight    float32
		Height    float32
		HeadParts []struct {
			v2FormRef
			Type uint32
		}
		HairColor   *v2Color
		FaceDetails *v2FormRef
		BodyTint    *v2Color
		FaceMorphs  []float64
		FaceParts   []int32
		Tints       []struct {
			v2Color
			Index, Type uint32
			Alpha       float32
		}
		DefaultRace *struct {
			Race   *v2FormRef
			Female bool
		}
	}
	if err := json.Unmarshal(payload, &a); err != nil {
		return nil, err
	}
	appearance := &pb.Appearance{Revision: a.Revision, Name: a.Name, Level: a.Level, Race: a.Race.proto(),
		Female: a.Female, Weight: a.Weight, Height: a.Height, HairColor: a.HairColor.proto(),
		FaceDetails: a.FaceDetails.proto(), BodyTint: a.BodyTint.proto(), FaceParts: a.FaceParts}
	for _, part := range a.HeadParts {
		appearance.HeadParts = append(appearance.HeadParts, &pb.HeadPart{Form: part.v2FormRef.proto(), Type: part.Type})
	}
	for _, morph := range a.FaceMorphs {
		appearance.FaceMorphs = append(appearance.FaceMorphs, float32(morph))
	}
	for _, tint := range a.Tints {
		appearance.Tints = append(appearance.Tints, &pb.Tint{Index: tint.Index, Type: tint.Type,
			Color: tint.v2Color.proto(), Alpha: tint.Alpha})
	}
	if a.DefaultRace != nil {
		appearance.DefaultRace = &pb.DefaultRace{Race: a.DefaultRace.Race.proto(), Female: a.DefaultRace.Female}
	}
	return []*pb.ClientMessage{{Message: &pb.ClientMessage_Appearance{Appearance: appearance}}}, nil
}

type v2Hand struct {
	Plugin     string
	FormID     uint32 `json:"formId"`
	Kind       string
	WeaponType uint32
}

func (h *v2Hand) proto() *pb.Hand {
	if h == nil {
		return nil
	}
	hand := &pb.Hand{WeaponType: h.WeaponType}
	switch h.Kind {
	case "weapon":
		hand.Kind = pb.HandKind_HAND_KIND_WEAPON
	case "torch":
		hand.Kind = pb.HandKind_HAND_KIND_TORCH
	case "spell":
		hand.Kind = pb.HandKind_HAND_KIND_SPELL
		return hand
	}
	hand.Form = &pb.FormRef{Plugin: h.Plugin, FormId: h.FormID}
	return hand
}

type v2Armor struct {
	v2FormRef
	SlotMask uint32
}

func armorProto(entries []v2Armor) []*pb.Armor {
	var armor []*pb.Armor
	for _, entry := range entries {
		armor = append(armor, &pb.Armor{Form: entry.v2FormRef.proto(), SlotMask: entry.SlotMask})
	}
	return armor
}

func convertV2Equipment(payload json.RawMessage) ([]*pb.ClientMessage, error) {
	var e struct {
		Revision      uint64
		Armor         []v2Armor
		RightHand     *v2Hand
		LeftHand      *v2Hand
		Ammo          *v2FormRef
		DefaultOutfit *struct {
			Armor []v2Armor
			Hands []v2Hand
			Ammo  *v2FormRef
		}
	}
	if err := json.Unmarshal(payload, &e); err != nil {
		return nil, err
	}
	equipment := &pb.Equipment{Revision: e.Revision, Armor: armorProto(e.Armor), RightHand: e.RightHand.proto(),
		LeftHand: e.LeftHand.proto(), Ammo: e.Ammo.proto()}
	if outfit := e.DefaultOutfit; outfit != nil && (len(outfit.Armor) > 0 || len(outfit.Hands) > 0 || outfit.Ammo != nil) {
		equipment.DefaultOutfit = &pb.DefaultOutfit{Armor: armorProto(outfit.Armor), Ammo: outfit.Ammo.proto()}
		for i := range outfit.Hands {
			equipment.DefaultOutfit.Hands = append(equipment.DefaultOutfit.Hands, outfit.Hands[i].proto())
		}
	}
	return []*pb.ClientMessage{{Message: &pb.ClientMessage_Equipment{Equipment: equipment}}}, nil
}
