package validate

import (
	"errors"
	"math"
	"slices"
	"strings"
	"unicode/utf8"

	"playerhub/server/internal/wire/pb"
)

const (
	MaximumPluginNameBytes = 260
	MaximumLocalFormID     = 0xFFFFFF
	MaximumHeading         = 4095
	MaximumPitch           = 1609
	MaximumSpeed           = 100000
	MaximumDirection       = 255
	MaximumTime            = 16383
	MaximumMovementState   = 6
	movementStateMask      = 0x7
	knownFlags             = 0x7FFF

	MaximumNameBytes        = 128
	MinimumDisplayNameBytes = 3
	MaximumDisplayNameBytes = 15
	SessionTokenBytes       = 32
	MaximumLevel            = 0xFFFF
	MaximumHeadParts        = 16
	MaximumHeadPartType     = 6
	FaceMorphCount          = 19
	FacePartCount           = 4
	MinimumFacePart         = -1
	MaximumFacePart         = 255
	MaximumTints            = 64
	MaximumTintIndex        = 0xFFFF
	MaximumTintType         = 14

	MaximumArmor        = 32
	MaximumOutfitArmor  = 8
	MaximumOutfitHands  = 2
	MinimumWeaponType   = 1
	MaximumWeaponType   = 9
	MaximumChatTextByte = 512

	MaximumVanished = 1024
)

var (
	ErrCheckpoint  = errors.New("invalid player transform")
	ErrContext     = errors.New("invalid player context")
	ErrAction      = errors.New("invalid animation action")
	ErrAppearance  = errors.New("invalid player appearance")
	ErrEquipment   = errors.New("invalid player equipment")
	ErrChat        = errors.New("invalid chat message")
	ErrDisplayName = errors.New("invalid display name")
	ErrToken       = errors.New("invalid session token")
	ErrVanish      = errors.New("invalid vanish")
	ErrInspect     = errors.New("invalid inspect")
)

var ErrUnexpected = errors.New("unexpected message")

func Message(m *pb.ClientMessage) error {
	if handled, err := trade(m); handled {
		return err
	}
	switch body := m.Message.(type) {
	case *pb.ClientMessage_Enter, *pb.ClientMessage_Exit, *pb.ClientMessage_Chat:
		return nil
	case *pb.ClientMessage_Checkpoint:
		return Checkpoint(body.Checkpoint)
	case *pb.ClientMessage_Context:
		return Context(body.Context)
	case *pb.ClientMessage_Action:
		return Action(body.Action)
	case *pb.ClientMessage_Appearance:
		return Appearance(body.Appearance)
	case *pb.ClientMessage_Equipment:
		return Equipment(body.Equipment)
	case *pb.ClientMessage_Vanish:
		if body.Vanish.GetSessionId() == 0 {
			return ErrVanish
		}
		return nil
	case *pb.ClientMessage_Inspect:
		if body.Inspect.GetSlot() == 0 {
			return ErrInspect
		}
		return nil
	default:
		return ErrUnexpected
	}
}

func Checkpoint(c *pb.Checkpoint) error {
	if c.GetHeading() > MaximumHeading || c.GetPitch() > MaximumPitch || c.GetPitch() < -MaximumPitch ||
		c.GetSpeed() > MaximumSpeed || c.GetDirection() > MaximumDirection || c.GetTime() > MaximumTime ||
		c.GetFlags()&^knownFlags != 0 || c.GetFlags()&movementStateMask > MaximumMovementState {
		return ErrCheckpoint
	}
	return nil
}

func WeaponDrawn(c *pb.Checkpoint) bool {
	return c.GetFlags()&uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN) != 0
}

func Context(c *pb.Context) error {
	if c.GetGraphEpoch() == 0 || (c.Furniture != nil && !formRef(c.Furniture)) ||
		c.GetShapeshift() < pb.Shapeshift_SHAPESHIFT_NONE || c.GetShapeshift() > pb.Shapeshift_SHAPESHIFT_WEREBEAR {
		return ErrContext
	}
	return nil
}

// VampireLordFlags reports whether the checkpoint has a flag only a vampire lord
// sets: levitating, Mist Form, or bats.
func VampireLordFlags(c *pb.Checkpoint) bool {
	const flags = pb.CheckpointFlag_CHECKPOINT_FLAG_LEVITATING | pb.CheckpointFlag_CHECKPOINT_FLAG_MIST_FORM |
		pb.CheckpointFlag_CHECKPOINT_FLAG_BATS
	return c.GetFlags()&uint32(flags) != 0
}

// BeastActionForm is the beast form whose graph plays the action: the werewolf's
// for the werewolf and the werebear, and the vampire lord's. SHAPESHIFT_NONE for an
// unknown action.
func BeastActionForm(a pb.BeastAction) pb.Shapeshift {
	switch {
	case a >= pb.BeastAction_BEAST_ACTION_WEREWOLF_ATTACK_RIGHT && a <= pb.BeastAction_BEAST_ACTION_WEREWOLF_FEED_PAIRED:
		return pb.Shapeshift_SHAPESHIFT_WEREWOLF
	case a >= pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_ATTACK_RIGHT &&
		a <= pb.BeastAction_BEAST_ACTION_VAMPIRE_LORD_CAST_INTERRUPT_RIGHT:
		return pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD
	}
	return pb.Shapeshift_SHAPESHIFT_NONE
}

// SameBeastGraph reports whether a form plays the actions of the given action form.
func SameBeastGraph(form, actionForm pb.Shapeshift) bool {
	if form == pb.Shapeshift_SHAPESHIFT_WEREBEAR {
		form = pb.Shapeshift_SHAPESHIFT_WEREWOLF
	}
	return actionForm != pb.Shapeshift_SHAPESHIFT_NONE && form == actionForm
}

func Action(a *pb.Action) error {
	if a.GetActionSequence() == 0 || a.GetGraphEpoch() == 0 || a.GetCheckpointSequence() == 0 {
		return ErrAction
	}
	if a.GetBeast() != pb.BeastAction_BEAST_ACTION_UNSPECIFIED {
		if BeastActionForm(a.GetBeast()) == pb.Shapeshift_SHAPESHIFT_NONE ||
			a.GetAction() != pb.AttackAction_ATTACK_ACTION_UNSPECIFIED || a.GetPower() ||
			a.GetDirection() != pb.PowerDirection_POWER_DIRECTION_NONE {
			return ErrAction
		}
		return nil
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
	for _, part := range a.GetFaceParts() {
		if part < MinimumFacePart || part > MaximumFacePart {
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

func Hello(h *pb.Hello) error {
	if token := h.GetSessionToken(); len(token) != 0 && len(token) != SessionTokenBytes {
		return ErrToken
	}
	if vanished := h.GetVanished(); len(vanished) > MaximumVanished || slices.Contains(vanished, 0) {
		return ErrVanish
	}
	return DisplayName(h.GetDisplayName())
}

func DisplayName(n string) error {
	if len(n) < MinimumDisplayNameBytes || len(n) > MaximumDisplayNameBytes {
		return ErrDisplayName
	}
	for i := range len(n) {
		if c := n[i]; !('a' <= c && c <= 'z' || 'A' <= c && c <= 'Z' || '0' <= c && c <= '9' || c == '_') {
			return ErrDisplayName
		}
	}
	return nil
}

func DisplayNameKey(n string) string {
	return strings.ToLower(n)
}

func Chat(c *pb.ChatSend) (string, error) {
	text := strings.Trim(c.GetText(), " ")
	if _, known := pb.ChatChannel_name[int32(c.GetChannel())]; !known ||
		text == "" || len(text) > MaximumChatTextByte || !utf8.ValidString(text) || hasControl(text) {
		return "", ErrChat
	}
	if whisper := c.GetChannel() == pb.ChatChannel_CHAT_CHANNEL_WHISPER; whisper != (c.GetRecipient() != 0) {
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
		if h.Form == nil || !baseGameFormRef(h.Form) || h.CastingArt != nil || h.CastingLight != nil {
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
	if h.GetKind() == pb.HandKind_HAND_KIND_SPELL {
		return optionalFormRef(h.Form) && h.GetWeaponType() == 0 && optionalFormRef(h.CastingArt) &&
			optionalFormRef(h.CastingLight)
	}
	if h.CastingArt != nil || h.CastingLight != nil {
		return false
	}
	switch h.GetKind() {
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

func optionalFormRef(f *pb.FormRef) bool {
	return f == nil || formRef(f)
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
