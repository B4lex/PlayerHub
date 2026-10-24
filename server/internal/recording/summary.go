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
			if beast, ok := body.Action.GetKind().(*pb.Action_Beast); ok {
				markers = append(markers, Marker{m.TMs, "attack", enumLabel(beast.Beast.String(), "BEAST_ACTION_")})
				continue
			}
			attack := body.Action.GetAttack()
			name, known := attackNames[attack.GetAction()]
			if !known {
				continue
			}
			label := strings.ToUpper(name[:1]) + name[1:]
			if direction := attack.GetDirection(); direction != pb.PowerDirection_POWER_DIRECTION_NONE {
				label = fmt.Sprintf("Power %s (%s)", name,
					strings.ToLower(strings.TrimPrefix(direction.String(), "POWER_DIRECTION_")))
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
				if label := area(c); label != area(previousContext) {
					markers = append(markers, Marker{m.TMs, "area", "Entered " + label})
				}
			}
			previousContext = c
		}
	}
	return markers
}

func area(c *pb.Context) string {
	if w := c.GetWorldspace(); w != nil {
		return fmt.Sprintf("worldspace %s:0x%06x", w.GetPlugin(), w.GetFormId())
	}
	cell := c.GetInteriorCell()
	return fmt.Sprintf("interior %s:0x%06x", cell.GetPlugin(), cell.GetFormId())
}

func enumLabel(value, prefix string) string {
	words := strings.ToLower(strings.ReplaceAll(strings.TrimPrefix(value, prefix), "_", " "))
	return strings.ToUpper(words[:1]) + words[1:]
}
