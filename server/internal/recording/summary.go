package recording

import (
	"fmt"
	"strings"

	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire/pb"
)

// Marker is a notable moment for the admin timeline.
type Marker struct {
	TMs int64 `json:"tMs"`
	// attack, appearance, equipment, weapon, furniture, or area.
	Kind  string `json:"kind"`
	Label string `json:"label"`
}

var attackNames = map[pb.AttackAction]string{
	pb.AttackAction_ATTACK_ACTION_NORMAL: "attack",
	pb.AttackAction_ATTACK_ACTION_LEFT:   "left-hand attack",
	pb.AttackAction_ATTACK_ACTION_DUAL:   "dual attack",
	pb.AttackAction_ATTACK_ACTION_BASH:   "bash",
}

// Markers lists attacks, gear changes, drawing or sheathing, furniture use,
// and area changes.
func Markers(r *Recording) []Marker {
	markers := []Marker{}
	seenAppearance, seenEquipment := false, false
	var previousCheckpoint *pb.Checkpoint
	var previousContext *pb.Context
	for _, m := range r.Messages {
		switch body := m.Message.Message.(type) {
		case *pb.ClientMessage_Action:
			a := body.Action
			name := attackNames[a.Action]
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

// area identifies where a context is. Exterior cells change constantly while
// walking, so only the worldspace counts there.
func area(c *pb.Context) (string, uint32) {
	if c.WorldspaceId != 0 {
		return "worldspace", c.WorldspaceId
	}
	return "interior", c.CellId
}
