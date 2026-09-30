// Package validate checks untrusted client messages field by field. It knows
// nothing about the sender's previous state; ordering rules such as rising
// sequences live in the hub.
package validate

import (
	"errors"
	"math"
	"strings"
	"unicode/utf8"

	"skyrim-mp/server/internal/wire/pb"
)

const (
	MaximumPluginNameBytes = 260
	MaximumLocalFormID     = 0xFFFFFF
	MaximumHeading         = 4095
	MaximumPitch           = 1609
	MaximumSpeed           = 100000
	MaximumDirection       = 255
	MaximumMovementState   = 6
	movementStateMask      = 0x7
	knownFlags             = 0xFFF

	MaximumNameBytes    = 128
	MaximumLevel        = 0xFFFF
	MaximumHeadParts    = 16
	MaximumHeadPartType = 6
	FaceMorphCount      = 19
	FacePartCount       = 4
	MaximumTints        = 64
	MaximumTintIndex    = 0xFFFF
	MaximumTintType     = 14

	MaximumArmor        = 32
	MaximumOutfitArmor  = 8
	MaximumOutfitHands  = 2
	MinimumWeaponType   = 1
	MaximumWeaponType   = 9
	MaximumChatTextByte = 512
)

// Errors name the rejected message, as the Python server's acknowledgements did.
var (
	ErrCheckpoint = errors.New("invalid player transform")
	ErrContext    = errors.New("invalid player context")
	ErrAction     = errors.New("invalid animation action")
	ErrAppearance = errors.New("invalid player appearance")
	ErrEquipment  = errors.New("invalid player equipment")
	ErrChat       = errors.New("invalid chat message")
)

func Checkpoint(c *pb.Checkpoint) error {
	if c.GetHeading() > MaximumHeading || c.GetPitch() > MaximumPitch || c.GetPitch() < -MaximumPitch ||
		c.GetSpeed() > MaximumSpeed || c.GetDirection() > MaximumDirection ||
		c.GetFlags()&^knownFlags != 0 || c.GetFlags()&movementStateMask > MaximumMovementState {
		return ErrCheckpoint
	}
	return nil
}

func WeaponDrawn(c *pb.Checkpoint) bool {
	return c.GetFlags()&uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN) != 0
}

func Context(c *pb.Context) error {
	if c.GetGraphEpoch() == 0 || (c.Furniture != nil && !formRef(c.Furniture)) {
		return ErrContext
	}
	return nil
}

func Action(a *pb.Action) error {
	if a.GetActionSequence() == 0 || a.GetGraphEpoch() == 0 || a.GetCheckpointSequence() == 0 {
		return ErrAction
	}
	if a.GetAction() < pb.AttackAction_ATTACK_ACTION_NORMAL || a.GetAction() > pb.AttackAction_ATTACK_ACTION_BASH {
		return ErrAction
	}
	direction := a.GetDirection()
	if a.GetPower() {
		if direction < pb.PowerDirection_POWER_DIRECTION_STANDING || direction > pb.PowerDirection_POWER_DIRECTION_RIGHT {
			return ErrAction
		}
	} else if direction != pb.PowerDirection_POWER_DIRECTION_NONE {
		return ErrAction
	}
	return nil
}

func Appearance(a *pb.Appearance) error {
	if a.GetRevision() == 0 || !name(a.GetName()) || a.GetLevel() > MaximumLevel || a.Race == nil || !formRef(a.Race) ||
		!between(a.GetWeight(), 0, 100) || !between(a.GetHeight(), 0.25, 4) {
		return ErrAppearance
	}
	if len(a.GetHeadParts()) > MaximumHeadParts {
		return ErrAppearance
	}
	for _, part := range a.GetHeadParts() {
		if part.Form == nil || !formRef(part.Form) || part.GetType() > MaximumHeadPartType {
			return ErrAppearance
		}
	}
	if (a.HairColor != nil && !color(a.HairColor)) || (a.FaceDetails != nil && !formRef(a.FaceDetails)) ||
		a.BodyTint == nil || !color(a.BodyTint) {
		return ErrAppearance
	}
	if len(a.GetFaceMorphs()) != FaceMorphCount || len(a.GetFaceParts()) != FacePartCount {
		return ErrAppearance
	}
	for _, morph := range a.GetFaceMorphs() {
		if !finite(morph) {
			return ErrAppearance
		}
	}
	if len(a.GetTints()) > MaximumTints {
		return ErrAppearance
	}
	for _, tint := range a.GetTints() {
		if tint.GetIndex() > MaximumTintIndex || tint.GetType() > MaximumTintType || tint.Color == nil ||
			!color(tint.Color) || !between(tint.GetAlpha(), 0, 1) {
			return ErrAppearance
		}
	}
	if race := a.DefaultRace; race != nil && (race.Race == nil || !baseGameFormRef(race.Race)) {
		return ErrAppearance
	}
	return nil
}

func Equipment(e *pb.Equipment) error {
	if e.GetRevision() == 0 || len(e.GetArmor()) > MaximumArmor {
		return ErrEquipment
	}
	type key struct {
		plugin string
		formID uint32
	}
	seen := make(map[key]struct{}, len(e.GetArmor()))
	for _, armor := range e.GetArmor() {
		if !armorEntry(armor) {
			return ErrEquipment
		}
		k := key{armor.Form.GetPlugin(), armor.Form.GetFormId()}
		if _, duplicate := seen[k]; duplicate {
			return ErrEquipment
		}
		seen[k] = struct{}{}
	}
	if (e.RightHand != nil && !hand(e.RightHand, false)) || (e.LeftHand != nil && !hand(e.LeftHand, true)) {
		return ErrEquipment
	}
	if right := e.RightHand; right != nil && twoHanded(right) && e.LeftHand != nil {
		return ErrEquipment
	}
	if e.Ammo != nil && !formRef(e.Ammo) {
		return ErrEquipment
	}
	if outfit := e.DefaultOutfit; outfit != nil && !defaultOutfit(outfit) {
		return ErrEquipment
	}
	return nil
}

// ChatText trims surrounding spaces and returns the text to relay.
func ChatText(text string) (string, error) {
	text = strings.Trim(text, " ")
	if text == "" || len(text) > MaximumChatTextByte || !utf8.ValidString(text) || hasControl(text) {
		return "", ErrChat
	}
	return text, nil
}

func defaultOutfit(o *pb.DefaultOutfit) bool {
	if len(o.GetArmor()) > MaximumOutfitArmor || len(o.GetHands()) > MaximumOutfitHands {
		return false
	}
	for _, armor := range o.GetArmor() {
		if !armorEntry(armor) || !baseGameFormRef(armor.Form) {
			return false
		}
	}
	type key struct {
		kind       pb.HandKind
		weaponType uint32
	}
	seen := make(map[key]struct{}, len(o.GetHands()))
	for _, h := range o.GetHands() {
		// Outfit hands are not tied to a hand, so placement rules do not apply.
		switch h.GetKind() {
		case pb.HandKind_HAND_KIND_WEAPON:
			if h.GetWeaponType() < MinimumWeaponType || h.GetWeaponType() > MaximumWeaponType {
				return false
			}
		case pb.HandKind_HAND_KIND_TORCH:
			if h.GetWeaponType() != 0 {
				return false
			}
		default:
			return false
		}
		if h.Form == nil || !baseGameFormRef(h.Form) {
			return false
		}
		k := key{h.GetKind(), h.GetWeaponType()}
		if _, duplicate := seen[k]; duplicate {
			return false
		}
		seen[k] = struct{}{}
	}
	return o.Ammo == nil || baseGameFormRef(o.Ammo)
}

func armorEntry(a *pb.Armor) bool {
	return a.Form != nil && formRef(a.Form) && a.GetSlotMask() != 0
}

func hand(h *pb.Hand, left bool) bool {
	switch h.GetKind() {
	case pb.HandKind_HAND_KIND_SPELL:
		// Every receiver shows the same placeholder spell, so none is sent.
		return h.Form == nil && h.GetWeaponType() == 0
	case pb.HandKind_HAND_KIND_TORCH:
		return left && h.Form != nil && formRef(h.Form) && h.GetWeaponType() == 0
	case pb.HandKind_HAND_KIND_WEAPON:
		weaponType := h.GetWeaponType()
		return h.Form != nil && formRef(h.Form) && weaponType >= MinimumWeaponType &&
			weaponType <= MaximumWeaponType && !(left && twoHanded(h))
	default:
		return false
	}
}

// twoHanded reports WEAPON_TYPE two-handed sword, axe, bow, and crossbow.
func twoHanded(h *pb.Hand) bool {
	if h.GetKind() != pb.HandKind_HAND_KIND_WEAPON {
		return false
	}
	switch h.GetWeaponType() {
	case 5, 6, 7, 9:
		return true
	}
	return false
}

func formRef(f *pb.FormRef) bool {
	plugin := f.GetPlugin()
	return len(plugin) >= 1 && len(plugin) <= MaximumPluginNameBytes && f.GetFormId() <= MaximumLocalFormID
}

func baseGameFormRef(f *pb.FormRef) bool {
	if !formRef(f) {
		return false
	}
	_, ok := baseGamePlugins[strings.ToLower(f.GetPlugin())]
	return ok
}

func color(c *pb.Color) bool {
	return c.GetR() <= 0xFF && c.GetG() <= 0xFF && c.GetB() <= 0xFF
}

func name(n string) bool {
	return len(n) <= MaximumNameBytes && utf8.ValidString(n) && !hasControl(n)
}

func hasControl(s string) bool {
	for _, r := range s {
		if r < 0x20 || r == 0x7F {
			return true
		}
	}
	return false
}

func finite(v float32) bool {
	f := float64(v)
	return !math.IsNaN(f) && !math.IsInf(f, 0)
}

func between(v float32, minimum, maximum float32) bool {
	return finite(v) && v >= minimum && v <= maximum
}
