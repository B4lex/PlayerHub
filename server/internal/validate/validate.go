package validate

import (
	"encoding/binary"
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
	knownFlags             = 0xFFFFFF

	SkeletonLayoutHuman       = 2
	SkeletonLayoutHumanChange = 3 // a change from the stream's last pose (wire.ResolveSkeletonPose)
	SkeletonBoneCount         = 63
	skeletonFirstTwistBone    = 23
	skeletonFirstFingerBone   = 31
	skeletonFirstHeldBone     = 61
	skeletonTranslationBytes  = 3 * 2
	maximumSkeletonMaskBytes  = 9
	allSkeletonBones          = 1<<SkeletonBoneCount - 1
	MaximumSkeletonPoseSize   = 219
	MaximumPosePlayers        = 32

	MaximumNameBytes        = 128
	MinimumDisplayNameBytes = 3
	MaximumDisplayNameBytes = 15
	SessionTokenBytes       = 32
	MaximumLevel            = 0xFFFF
	MaximumHeadParts        = 32
	FaceMorphCount          = 19
	FacePartCount           = 4
	MinimumFacePart         = -1
	MaximumFacePart         = 255
	MaximumTints            = 64
	MaximumTintIndex        = 0xFFFF
	MaximumTintType         = 14
	MaximumOverlays         = 32
	MaximumTexturePathBytes = 260
	MaximumGlowStrength     = 100

	MaximumArmor        = 32
	MaximumOutfitArmor  = 8
	MaximumBeastArmor   = 8
	MaximumOutfitHands  = 2
	MaximumEffects      = 32
	MaximumMagnitude    = 1e6
	MinimumWeaponType   = 1
	MaximumWeaponType   = 9
	StaffWeaponType     = 8
	MaximumChatTextRune = 512
	MaximumChatTextByte = 4 * MaximumChatTextRune

	MaximumVanished = 1024

	MaximumIdleEventBytes = 64
	idleEventPrefix       = "Idle"
	offsetStopEvent       = "OffsetStop"
)

var (
	ErrCheckpoint  = errors.New("invalid player transform")
	ErrContext     = errors.New("invalid player context")
	ErrAction      = errors.New("invalid animation action")
	ErrAppearance  = errors.New("invalid player appearance")
	ErrEquipment   = errors.New("invalid player equipment")
	ErrEffects     = errors.New("invalid player effects")
	ErrChat        = errors.New("invalid chat message")
	ErrDisplayName = errors.New("invalid display name")
	ErrToken       = errors.New("invalid session token")
	ErrVanish      = errors.New("invalid vanish")
	ErrInspect     = errors.New("invalid inspect")
	ErrPoseLimit   = errors.New("invalid pose limit")
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
	case *pb.ClientMessage_Effects:
		return Effects(body.Effects)
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
	case *pb.ClientMessage_PoseLimit:
		if body.PoseLimit.GetPlayers() > MaximumPosePlayers {
			return ErrPoseLimit
		}
		return nil
	default:
		return ErrUnexpected
	}
}

func Checkpoint(c *pb.Checkpoint) error {
	if c.GetHeading() > MaximumHeading || c.GetPitch() > MaximumPitch || c.GetPitch() < -MaximumPitch ||
		c.GetLookPitch() > MaximumPitch || c.GetLookPitch() < -MaximumPitch ||
		c.GetSpeed() > MaximumSpeed || c.GetDirection() > MaximumDirection || c.GetTime() > MaximumTime ||
		c.GetFlags()&^knownFlags != 0 || c.GetFlags()&movementStateMask > MaximumMovementState ||
		!castFlags(c.GetFlags()) || !shoutFlags(c.GetFlags()) || !SkeletonPose(c.GetSkeleton()) {
		return ErrCheckpoint
	}
	return nil
}

// castFlags reports whether each release flag comes with its hand's casting flag, and the dual
// casting flag with the left hand's, which the dual cast runs on.
func castFlags(flags uint32) bool {
	requires := func(flag, casting pb.CheckpointFlag) bool {
		return flags&uint32(flag) == 0 || flags&uint32(casting) != 0
	}
	return requires(pb.CheckpointFlag_CHECKPOINT_FLAG_CAST_RELEASED_LEFT, pb.CheckpointFlag_CHECKPOINT_FLAG_CASTING_LEFT) &&
		requires(pb.CheckpointFlag_CHECKPOINT_FLAG_CAST_RELEASED_RIGHT, pb.CheckpointFlag_CHECKPOINT_FLAG_CASTING_RIGHT) &&
		requires(pb.CheckpointFlag_CHECKPOINT_FLAG_DUAL_CASTING, pb.CheckpointFlag_CHECKPOINT_FLAG_CASTING_LEFT)
}

// shoutFlags reports whether the release and word flags come with the shouting flag, at most
// one word flag is set, and the failure flag comes with the release flag.
func shoutFlags(flags uint32) bool {
	const (
		shouting = uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SHOUTING)
		released = uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SHOUT_RELEASED)
		twoWords = uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SHOUT_TWO_WORDS)
		three    = uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SHOUT_THREE_WORDS)
		failed   = uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_SHOUT_FAILED)
		shout    = released | twoWords | three
	)
	return (flags&shout == 0 || flags&shouting != 0) && flags&(twoWords|three) != twoWords|three &&
		(flags&failed == 0 || flags&released != 0)
}

// SkeletonPose reports whether b is empty or a layout byte, the shortest varint of a mask of
// missing bones below bit SkeletonBoneCount, the root translation, and the bit-packed rotations of
// the present bones.
func SkeletonPose(b []byte) bool {
	if len(b) == 0 {
		return true
	}
	missing, n, ok := SkeletonHeader(b, SkeletonLayoutHuman)
	if !ok {
		return false
	}
	bits := 0
	for bone := range SkeletonBoneCount {
		if missing&(1<<bone) == 0 {
			bits += skeletonRotationBits(bone)
		}
	}
	return len(b) == n+skeletonTranslationBytes+(bits+7)/8
}

// SkeletonHeader reads a pose's layout byte, which must be layout, and the shortest varint of its
// mask of missing bones below bit SkeletonBoneCount; n is the header's size.
func SkeletonHeader(b []byte, layout byte) (missing uint64, n int, ok bool) {
	if len(b) == 0 || b[0] != layout {
		return 0, 0, false
	}
	var shortest [binary.MaxVarintLen64]byte
	missing, n = binary.Uvarint(b[1:min(len(b), 1+maximumSkeletonMaskBytes)])
	if n <= 0 || n != binary.PutUvarint(shortest[:], missing) || missing&^allSkeletonBones != 0 {
		return 0, 0, false
	}
	return missing, 1 + n, true
}

// skeletonRotationBits counts a bone's two-bit index of its largest component and its three stored components.
func skeletonRotationBits(bone int) int {
	return 2 + 3*SkeletonComponentBits(bone)
}

// SkeletonComponentBits is the width of each stored quaternion component of a bone.
func SkeletonComponentBits(bone int) int {
	switch {
	case bone < skeletonFirstTwistBone:
		return 9
	case bone >= skeletonFirstFingerBone && bone < skeletonFirstHeldBone:
		return 7
	}
	return 8
}

func WeaponDrawn(c *pb.Checkpoint) bool {
	return c.GetFlags()&uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_WEAPON_DRAWN) != 0
}

func Context(c *pb.Context) error {
	if c.GetGraphEpoch() == 0 || (c.Worldspace != nil && c.InteriorCell != nil) ||
		!optionalFormRef(c.InteriorCell) || !optionalFormRef(c.Worldspace) || !optionalFormRef(c.Furniture) ||
		c.GetShapeshift() < pb.Shapeshift_SHAPESHIFT_NONE || c.GetShapeshift() > pb.Shapeshift_SHAPESHIFT_WEREBEAR {
		return ErrContext
	}
	// Furniture and beast forms play their own idles on every peer.
	if c.GetIdle() != "" &&
		(!HeldIdleEvent(c.GetIdle()) || c.Furniture != nil || c.GetShapeshift() != pb.Shapeshift_SHAPESHIFT_NONE) {
		return ErrContext
	}
	return nil
}

// HeldIdleEvent reports whether a name is an idle graph event a player can be held
// in: the Idle prefix in any case, as graph events compare, then identifier
// characters, at most MaximumIdleEventBytes in all. Receivers send it to a graph.
func HeldIdleEvent(name string) bool {
	if len(name) <= len(idleEventPrefix) || len(name) > MaximumIdleEventBytes ||
		!strings.EqualFold(name[:len(idleEventPrefix)], idleEventPrefix) {
		return false
	}
	for i := 0; i < len(name); i++ {
		if c := name[i]; !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	return true
}

// IdleActionEvent reports whether a name may be relayed as an idle action: a held
// idle event, or OffsetStop, which ends the carry-arm overlays idle clips start.
func IdleActionEvent(name string) bool {
	return HeldIdleEvent(name) || strings.EqualFold(name, offsetStopEvent)
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
	switch kind := a.GetKind().(type) {
	case *pb.Action_Beast:
		if BeastActionForm(kind.Beast) == pb.Shapeshift_SHAPESHIFT_NONE {
			return ErrAction
		}
		return nil
	case *pb.Action_Idle:
		if !IdleActionEvent(kind.Idle) {
			return ErrAction
		}
		return nil
	case *pb.Action_Power:
		if kind.Power == nil || !formRef(kind.Power) {
			return ErrAction
		}
		return nil
	case *pb.Action_Attack:
		attack := kind.Attack
		if attack.GetAction() < pb.AttackAction_ATTACK_ACTION_NORMAL || attack.GetAction() > pb.AttackAction_ATTACK_ACTION_BASH ||
			attack.GetDirection() < pb.PowerDirection_POWER_DIRECTION_NONE ||
			attack.GetDirection() > pb.PowerDirection_POWER_DIRECTION_RIGHT {
			return ErrAction
		}
		return nil
	}
	return ErrAction
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
		if part.Form == nil || !formRef(part.Form) {
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
			!color(tint.Color) || !between(tint.GetAlpha(), 0, 1) || !texturePath(tint.GetTexture()) {
			return ErrAppearance
		}
	}
	if len(a.GetOverlays()) > MaximumOverlays {
		return ErrAppearance
	}
	for _, overlay := range a.GetOverlays() {
		if overlay.GetArea() < pb.OverlayArea_OVERLAY_AREA_BODY || overlay.GetArea() > pb.OverlayArea_OVERLAY_AREA_FACE ||
			!texturePath(overlay.GetTexture()) || overlay.Color == nil || !color(overlay.Color) ||
			!between(overlay.GetAlpha(), 0, 1) || overlay.GlowColor == nil || !color(overlay.GlowColor) ||
			!between(overlay.GetGlowStrength(), 0, MaximumGlowStrength) {
			return ErrAppearance
		}
	}
	if a.DefaultRace != nil && !baseGameFormRef(a.DefaultRace) {
		return ErrAppearance
	}
	return nil
}

func Equipment(e *pb.Equipment) error {
	if e.GetRevision() == 0 || len(e.GetArmor()) > MaximumArmor {
		return ErrEquipment
	}
	if !armorList(e.GetArmor()) || len(e.GetBeastArmor()) > MaximumBeastArmor || !armorList(e.GetBeastArmor()) {
		return ErrEquipment
	}
	if (e.RightHand != nil && !hand(e.RightHand, false)) || (e.LeftHand != nil && !hand(e.LeftHand, true)) {
		return ErrEquipment
	}
	if right := e.RightHand; right != nil && twoHanded(right) && e.LeftHand != nil {
		return ErrEquipment
	}
	for _, beast := range []*pb.Hand{e.BeastRightHand, e.BeastLeftHand} {
		if beast != nil && (beast.GetKind() != pb.HandKind_HAND_KIND_SPELL || !hand(beast, false)) {
			return ErrEquipment
		}
	}
	if (e.Ammo != nil && !formRef(e.Ammo)) || (e.Shout != nil && !formRef(e.Shout)) {
		return ErrEquipment
	}
	if outfit := e.DefaultOutfit; outfit != nil && !defaultOutfit(outfit) {
		return ErrEquipment
	}
	return nil
}

func Effects(e *pb.Effects) error {
	if e.GetRevision() == 0 || len(e.GetEffects()) > MaximumEffects {
		return ErrEffects
	}
	type key struct {
		plugin string
		formID uint32
	}
	seen := make(map[key]struct{}, len(e.GetEffects()))
	for _, effect := range e.GetEffects() {
		if effect.Effect == nil || !formRef(effect.Effect) ||
			!between(effect.GetMagnitude(), -MaximumMagnitude, MaximumMagnitude) {
			return ErrEffects
		}
		k := key{effect.Effect.GetPlugin(), effect.Effect.GetFormId()}
		if _, duplicate := seen[k]; duplicate {
			return ErrEffects
		}
		seen[k] = struct{}{}
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
		text == "" || len(text) > MaximumChatTextByte || !utf8.ValidString(text) ||
		utf8.RuneCountInString(text) > MaximumChatTextRune || hasControl(text) {
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
		if h.Form == nil || !baseGameFormRef(h.Form) || h.CastingArt != nil || h.CastingLight != nil ||
			h.Enchantment != nil {
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

func armorList(entries []*pb.Armor) bool {
	type key struct {
		plugin string
		formID uint32
	}
	seen := make(map[key]struct{}, len(entries))
	for _, armor := range entries {
		if !armorEntry(armor) {
			return false
		}
		k := key{armor.Form.GetPlugin(), armor.Form.GetFormId()}
		if _, duplicate := seen[k]; duplicate {
			return false
		}
		seen[k] = struct{}{}
	}
	return true
}

func armorEntry(a *pb.Armor) bool {
	return a.Form != nil && formRef(a.Form) && a.GetSlotMask() != 0
}

func hand(h *pb.Hand, left bool) bool {
	look := optionalFormRef(h.CastingArt) && optionalFormRef(h.CastingLight)
	if h.GetKind() == pb.HandKind_HAND_KIND_SPELL {
		return optionalFormRef(h.Form) && h.GetWeaponType() == 0 && look && h.Enchantment == nil
	}
	staff := h.GetKind() == pb.HandKind_HAND_KIND_WEAPON && h.GetWeaponType() == StaffWeaponType
	if staff && !(look && optionalFormRef(h.Enchantment)) {
		return false
	}
	if !staff && (h.CastingArt != nil || h.CastingLight != nil || h.Enchantment != nil) {
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

// texturePath accepts a relative .dds path in printable ASCII that cannot leave
// the receiver's data folder.
func texturePath(p string) bool {
	if p == "" || len(p) > MaximumTexturePathBytes || !strings.HasSuffix(strings.ToLower(p), ".dds") ||
		p[0] == '\\' || p[0] == '/' {
		return false
	}
	for i := 0; i < len(p); i++ {
		if p[i] < 0x20 || p[i] > 0x7E || p[i] == ':' {
			return false
		}
	}
	return !slices.Contains(strings.FieldsFunc(p, func(r rune) bool { return r == '\\' || r == '/' }), "..")
}

func finite(v float32) bool {
	f := float64(v)
	return !math.IsNaN(f) && !math.IsInf(f, 0)
}

func between(v float32, minimum, maximum float32) bool {
	return finite(v) && v >= minimum && v <= maximum
}
