package recording

import (
	"fmt"
	"strings"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

type Marker struct {
	TMs   int64  `json:"tMs"`
	Kind  string `json:"kind"`
	Label string `json:"label"`
}

var attackNames = map[pb.AttackAction]string{
	pb.AttackAction_ATTACK_ACTION_NORMAL: "attack",
	pb.AttackAction_ATTACK_ACTION_LEFT:   "left-hand attack",
	pb.AttackAction_ATTACK_ACTION_DUAL:   "dual attack",
	pb.AttackAction_ATTACK_ACTION_BASH:   "bash",
}

func Markers(r *Recording) []Marker {
	markers := []Marker{}
	seenAppearance, seenEquipment := false, false
	var previousCheckpoint *pb.Checkpoint
	var previousContext *pb.Context
	for _, m := range r.Messages {
		switch body := m.Message.Message.(type) {
		case *pb.ClientMessage_Action:
			a := body.Action
			if a.Beast != pb.BeastAction_BEAST_ACTION_UNSPECIFIED {
				markers = append(markers, Marker{m.TMs, "attack", enumLabel(a.Beast.String(), "BEAST_ACTION_")})
				continue
			}
			name, known := attackNames[a.Action]
			if !known {
				continue
			}
			label := strings.ToUpper(name[:1]) + name[1:]
			if a.Power {
				direction := strings.ToLower(strings.TrimPrefix(a.Direction.String(), "POWER_DIRECTION_"))
				label = fmt.Sprintf("Power %s (%s)", name, direction)
			}
			markers = append(markers, Marker{m.TMs, "attack", label})
		case *pb.ClientMessage_Appearance:
			if seenAppearance {
				markers = append(markers, Marker{m.TMs, "appearance", "Appearance changed"})
			}
			seenAppearance = true
		case *pb.ClientMessage_Equipment:
			if seenEquipment {
				markers = append(markers, Marker{m.TMs, "equipment", "Equipment changed"})
			}
			seenEquipment = true
		case *pb.ClientMessage_Checkpoint:
			c := body.Checkpoint
			if previousCheckpoint != nil && validate.WeaponDrawn(c) != validate.WeaponDrawn(previousCheckpoint) {
				label := "Weapon sheathed"
				if validate.WeaponDrawn(c) {
					label = "Weapon drawn"
				}
				markers = append(markers, Marker{m.TMs, "weapon", label})
			}
			previousCheckpoint = c
		case *pb.ClientMessage_Context:
			c := body.Context
			if previousContext != nil {
				if (c.Furniture == nil) != (previousContext.Furniture == nil) {
					label := "Started using furniture"
					if c.Furniture == nil {
						label = "Left furniture"
					}
					markers = append(markers, Marker{m.TMs, "furniture", label})
				}
				if c.GetShapeshift() != previousContext.GetShapeshift() {
					label := "Returned to human form"
					if c.GetShapeshift() != pb.Shapeshift_SHAPESHIFT_NONE {
						label = "Transformed into " + strings.ToLower(strings.ReplaceAll(
							strings.TrimPrefix(c.GetShapeshift().String(), "SHAPESHIFT_"), "_", " "))
					}
					markers = append(markers, Marker{m.TMs, "shapeshift", label})
				}
				kind, id := area(c)
				previousKind, previousID := area(previousContext)
				if kind != previousKind || id != previousID {
					markers = append(markers, Marker{m.TMs, "area", fmt.Sprintf("Entered %s 0x%08x", kind, id)})
				}
			}
			previousContext = c
		}
	}
	return markers
}

func area(c *pb.Context) (string, uint32) {
	if c.WorldspaceId != 0 {
		return "worldspace", c.WorldspaceId
	}
	return "interior", c.CellId
}

func enumLabel(value, prefix string) string {
	words := strings.ToLower(strings.ReplaceAll(strings.TrimPrefix(value, prefix), "_", " "))
	return strings.ToUpper(words[:1]) + words[1:]
}
