# Appearance and Equipment

## Status

Not run in game: the tint mask offset on AE 1.6.x and the face tint render on
SE 1.5.97 (ID 26454); head parts of mod-added types and the extra parts listed
on the receiver ([Actor base](#actor-base)); tint textures that differ from
the receiver's race asset; RaceMenu [overlays](#overlays).

## Scope

A remote actor looks like its source player's character, carries the player's
display name, and wears and wields what the source has equipped, from any
plugin the receiver also has. In a vanilla beast form the actor is that beast
([Shapeshifts](#shapeshifts)). What the receiver cannot resolve falls back to
vanilla data or the source's default look, so players with different
modpacks still see each other dressed and armed. A source can also publish the
default look in place of its own ([Always use](#always-use)).

The wire format is in [Network](network.md#appearance-and-equipment); how draws
and attacks wait for the hands in [Combat presentation](combat-presentation.md);
the settings in [Configuration](configuration.md#settings).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/appearance/AppearanceSync` | Capture and publish; receive, build the actor base, respawn requests, hair, skin, face tint, and overlay presentation |
| `actor/RaceMenuOverlays` | SKEE's overlay and override interfaces: capture the player's overlays, apply them to a remote actor, and release them ([Overlays](#overlays)) |
| `forms/TextureFiles` | `TextureExists`: whether the receiver's loose files or archives hold a texture, cached per path |
| `sync/equipment/EquipmentSync` | Capture and publish; receive, resolve, reconcile the actor's inventory and a vampire lord actor's spells and armor; hand readiness |
| `sync/equipment/SpellStandIn` | Capture a spell hand's spell and look; resolve it to the receiver's own spell or a runtime stand-in spell ([Combat presentation](combat-presentation.md#spell-look)) |
| `sync/equipment/FakeLight` | A shadow-casting light's runtime copy without its shadow flags |
| `sync/look/DefaultLook` | Resolve `[DefaultLook]`, including the preset face, and select the pieces for a capture |
| `sync/look/LookPreset` | List RaceMenu presets and parse one into its vanilla face data, without engine access ([Preset face](#preset-face)) |
| `sync/PlayerLabel.h` | `PlayerLabel::Format` |
| `forms/RaceForms` | `IsPlayableRace`; `GetPlayableRace`, the playable race a capture sends for a race ([Playable race](#playable-race)); a race's face data and default head part per type |
| `forms/BeastForms` | The vanilla beast races and their forms, including the vampire lord's armor |
| `sync/shapeshift/ShapeshiftSync` | The local form and levitation; beast actions both ways ([Combat presentation](combat-presentation.md#beast-actions)) |
| `protocol/AppearanceState.h`, `EquipmentState.h`, `FormReference.h` | Plain data and validators shared by capture and the network parser |
| `forms/FormLookup` | Encode and resolve `{plugin, formId}` |
| `forms/DynamicForm.h` | `DynamicForm::Create`: a factory-built form with a dynamic form ID |
| `forms/ItemFilter.h`, `ItemCatalog`, `ItemForms` | Item traits, filters, and the searchable catalog |

## Appearance

The actor shows the source's race, sex, body weight, height, head parts (face,
eyes, hair, beard, brows, scars), hair color, face morph sliders and presets,
face detail texture, skin tone, face tint layers (war paint, dirt, lip and
cheek color) with their textures, and RaceMenu overlays (body, hand, feet, and
face paint).

### Capture

On the game thread, once per second:

- `race`: the [playable race](#playable-race) of the actor base race; without
  one, `originalRace` when playable (it is the player record's template race,
  so it comes last); otherwise the capture is skipped. In a vanilla beast form
  the last revision stands and the form travels apart
  ([Shapeshifts](#shapeshifts)).
- `level`: `Actor::GetLevel`, shown only in the
  [player menu](ui-overlay.md#player-menu). `name` is left empty: the server
  sets the [display name](#display-name).
- `defaultRace`: `[DefaultLook] Race` when `race` is not from a base-game
  plugin, else unset; `defaultFace`, the
  [preset face](#preset-face), is set with it when a preset is configured.
  With `AlwaysUse` the capture is the default look instead
  ([Always use](#always-use)).
- `height`: `TESNPC::height` × the reference scale (`refScale / 100`) × the
  local scale of the third-person skeleton's `NPC` node rounded to 0.01,
  clamped to `0.25..4`. RaceMenu's height slider scales that node
  ([skeleton scale](#engine-assumptions)); without RaceMenu it stays 1. The
  race's height is left out, since the receiver's resolved race applies its
  own. Without 3D the last node scale stands. The remote plays the source's
  graph `Speed` unchanged ([graph speed](remote-actor.md#graph-speed)), so a
  scaled character's legs keep the source's pace.
- `female`, `weight`, `headParts[0..numHeadParts)`, the hair color
  form's RGB, `faceDetails`, and `faceData` morphs and parts. Extra parts are
  implied by their parent and skipped; the receiver lists them again
  ([Actor base](#actor-base)). A head part's `type` is `BGSHeadPart::type`
  (PNAM), a `uint32` sent as is, since RaceMenu add-ons use types outside the
  vanilla `0..6` (nails 41, tails 40;
  [head part types](#engine-assumptions)). At most 32 top-level parts are sent
  (`kMaximumHeadParts`); a capture over it logs one warning naming the first
  part left out. Presets are clamped to `-1..255` (`ClampFacePart`, `-1` none;
  [face presets](#engine-assumptions)).
- `bodyTint`: `TESNPC::bodyTintColor`, or the skin tone mask's color if black.
- `tints`: `PlayerCharacter` runtime `tintMasks` in blend order, through
  CommonLib's `GetPlayerRuntimeData` (`0xB10` on SE as `GetNumTints` ID 39614
  reads it, `0xB20` on 1.7.104 as ID 40700 does). Each mask maps to the race's
  `TintAsset` for the sex by texture (pointer, then case-insensitive path) for
  its stable `index`; zero-alpha masks and masks without an asset are skipped.
  `texture` is the mask's `TESTexture::textureName`, since RaceMenu shows an
  alternative or add-on war paint by rewriting the race asset's path
  ([tint textures](#engine-assumptions)), which the index alone would lose. A
  path failing the [texture path](network.md#texture-paths) rule is skipped and
  logged once.
- `overlays`: `RaceMenuOverlays::Capture` ([Overlays](#overlays)).

A changed capture must repeat in two consecutive captures, so dragging an
editor slider does not stream updates. A look that keeps changing for 10 s
(`kUnsettledCaptureWarningSeconds`) logs one warning per streak naming the
first differing value of each changed field. A settled look failing
`IsValidAppearance` is never published; the first failing field and value is
logged (`DescribeInvalidAppearance`) once per reason. Publishes are at least
3 s apart, each increments `revision`, and the last is republished on every new
connection.

### Playable race

`Forms::GetPlayableRace` returns a playable race as is, otherwise its morph
race (`TESRace::morphRace`, NAM8) when that is playable. Every vanilla vampire
race (`NordRaceVampire` `088794`, ...) is not playable and names its own race
there, so a vampire character is captured, for appearance and equipment, as
its own race. The beast races name no playable morph race (the werebear names
`WerewolfBeastRace`), and only one step is taken, so transformations stay
unplayable.

### Actor base

Each accepted revision gets a new runtime `TESNPC` from the form factory with a
dynamic form ID (`TESDataHandler::GetNextID`), built from engine defaults
rather than a template record, so remote actors depend on no particular base
form. It has no class, AI packages, factions, inventory, or outfits, and its
`faceData` points at a shared engine default that is replaced, never written.
The base then gets:

- the `kUnique` flag: a remote player is one character, not a generic NPC;
- the full name set to the player label, read by the crosshair and other name
  displays;
- the race's default voice type for its sex when `Skyrim.esm` `VoicesPlayer`
  (`0x068ACA`) holds it, else `MaleEvenToned` (`0x013AD2`) or
  `FemaleEvenToned` (`0x013ADD`), since the shouted words' dialogue requires a
  voice in that list
  ([Combat presentation](combat-presentation.md#shout-and-power-receiver)) and
  the `PlayerVoiceMale`/`PlayerVoiceFemale` default objects are empty; every
  other line is suppressed
  ([Remote actor](remote-actor.md#reaction-suppression));
- race and original race, sex, weight, and height clamped to `0.85..1.15`
  (`kMinimumRemoteHeight`, `kMaximumRemoteHeight`), so an extreme scale cannot
  make a giant or a doll in another player's world;
- the race's attack data map and the default combat style, which
  `TESNPC::InitItemImpl` fills only for NPCs loaded from plugins; weapon draws
  read the attack data map without a null check;
- the resolved head parts, each followed by its extra parts, hair color, face
  details, morphs, parts, body tint, and tint layers
  (`interpolationValue = alpha × 100`, with the matching preset). Each layer's
  texture is kept beside the base in `RemoteState::baseTintTextures`: the race
  asset's path when `texture` names the same file, else the sent path.

`TESNPC::InitItemImpl` lists a plugin NPC's extra parts after their parent, and
the head build attaches only listed parts ([head parts](#engine-assumptions)).
A runtime base skips `InitItemImpl`, so `ResolveHeadParts` lists the extras
itself (otherwise meshes a mod puts in extra parts, such as a hairline, would
be missing), following the engine's rules (`HeadPartList`): a second part of a
single-part type (face, eyes, hair, facial hair, brows) is skipped with its
extras, a listed part is not repeated, and the list stops at 127
(`TESNPC::numHeadParts` is an `int8_t`). The engine tracks types in a 32-bit
mask by `type` modulo 32, where a mod-added type can collide;
`HeadPartList` compares exact types.

Bases are never freed: a deleted reference may still point at its base for a
few frames, and the leak is bounded by the source's publish rate. A revision
that cannot be built keeps the previous base.

### Resolution and fallback

| Data | Unresolvable when | Fallback |
| --- | --- | --- |
| Race | plugin or form missing, not a race, or not playable | `defaultRace` if it resolves to a playable race, else `NordRace` (`Skyrim.esm` `013746`) |
| Head part | missing, wrong type, or its valid-race list rejects the race | the race's default part of that type and sex, else omitted; races have no default for a mod-added type, so such a part is left out |
| Face part | none after resolution | the race's default face |
| Hair color | color form creation fails | the race's default hair color |
| Face details | missing | the race's default face details |
| Tint layer | no race tint asset with that index and type, or a `texture` other than the asset's that `TextureExists` does not find | dropped |
| Overlay | no SKEE, no free overlay slot of its area, or a texture `TextureExists` does not find | dropped |

A fallback race takes the race's default look in the source's sex, or, when
the race is the sent `defaultRace` and `defaultFace` is set, that face in its
own sex (`MakeFaceAppearance`, each tint taking the texture of the race layer
its index and type name). `NordRace` never takes the face, since it was built
for another race. The source's head parts, tints, morphs, presets, face
details, and hair color belong to its own race and are dropped; display name,
level, weight, and height are kept. The base is then built as for an own race,
so the table above applies to the face's head parts and tints too. Each
fallback is logged once per revision.

### Spawn and respawn

- An actor waits up to 3 s after its first checkpoint for an appearance, then
  spawns with a default base: `NordRace` male with the race's default look and
  the [placeholder label](#player-label).
- A newer revision retires the actor at the next pass, at least 2 s after the
  previous respawn for that player. The respawn uses the new base and rebuilds
  locomotion, stance, and furniture from the played checkpoint. Coordinate-space
  respawns reuse the current base.
- A change of only display name or level adopts the revision without a new
  base or respawn; the built base's full name is rewritten in place.
- Once the respawned actor is ready with 3D, `Actor::UpdateHairColor` and
  `UpdateSkinColor` run once.
- The face tint: `PrepareHeadPartForShaders` (ID 26838, SE 26259) gives a
  runtime-built face a blank tint texture and fills it only for the player base
  or with `bUseFaceGenPreprocessedHeads` off, so a remote face would render
  dark. The receiver finds the face geometry (named after the face part's
  editor ID), takes its FaceGen material's tint texture, and queues the
  engine's tint render (ID 27040, SE 26454) with masks from the base's layers
  (texture from `baseTintTextures`, color, `interpolationValue / 100` alpha, at
  most 16 non-zero layers). The render reads only the mask's `textureName`
  ([tint render](#engine-assumptions)), so each mask is a stack `MaskTexture`
  with `TESTexture`'s layout and the asset's vtable. It renders again whenever
  the head gets a new tint texture, such as after a 3D reset. Each remote render
  is followed by a render of the local player's own tint list with no
  destination (`QueuePlayerFaceTintRestore`), because after a RaceMenu tint
  edit the player's face shows the shared tint render target itself
  ([tint render](#engine-assumptions)); without it, a remote face built while
  that holds would repaint the player's face with the remote player's skin and
  war paint.
- RaceMenu overlays are applied in the same pass as the colors, when the
  spawned revision is the latest ([Overlays](#overlays)).

### Overlays

Goal: body, hand, feet, and face paint and tattoos that RaceMenu (SKEE) puts on
overlay shapes show on the remote actor when both players have RaceMenu and the
texture, with the source's color, transparency, and glow. Other NiOverride data
(body morphs, node transforms, armor and skin overrides) is out of scope.

- **Interfaces:** `RaceMenuOverlays::Initialize` runs at SKSE's
  `kPostPostLoad`. It dispatches SKEE's interface exchange
  (`0x9E3779B9`) to `skee` and queries `Overlay` and `Override` from the
  returned map, both at version 2 or later; without them nothing is captured
  or applied, and a line logs why. The interfaces are declared locally after
  RaceMenu's `ModderResource/IPluginInterface.h`.
- **Capture:** for each area, slots `0..GetOverlayCount(Normal, area)` of the
  nodes `Body [Ovl#]`, `Hands [Ovl#]`, `Feet [Ovl#]`, and `Face [Ovl#]`, in
  that order, up to 32 overlays. Spell overlays (`[SOvl#]`) are left out. Each
  value is the node override for the player's sex, or the node's current
  property when no override is saved, as RaceMenu reads them: texture (key 9,
  index 0), tint color (7), alpha (8), glow color (0), and glow strength
  (emissive multiple, 1), the last four at index `-1`. A slot without a
  texture, with SKEE's default `actors\character\overlays\default.dds`, or
  with alpha 0 is skipped; a path that fails the
  [texture path](network.md#texture-paths) rule is skipped and logged once per
  path. Missing color, alpha, or glow default to white, 1, and none. Alpha is
  clamped to `0..1` and glow strength to `0..100`.
- **Apply:** `RaceMenuOverlays::Apply` fills the receiver's slots of each area
  from 0 in the order received, so a source's gaps close and layering order
  holds; an overlay beyond the receiver's slot count, or whose texture
  `TextureExists` does not find, is dropped. Each kept overlay becomes node
  overrides for the actor base's sex with the five values, then
  `AddOverlays` (immediate) installs the shapes and `SetNodeProperties`
  applies the overrides. SKEE keeps both per reference and reapplies them when
  the actor's 3D is rebuilt ([SKEE overlays](#engine-assumptions)). One debug
  line counts applied, missing, and slotless overlays and names the first
  missing texture.
- **Release:** a new revision respawns the actor, so overlays are only ever
  applied to a fresh reference. `ActorSpawner::Remove` calls
  `RaceMenuOverlays::Release` for an actor that got overlays:
  `RemoveAllNodeOverridesByReference` and a deferred `RemoveOverlays`, so
  SKEE's co-save does not keep the retired reference.

### Shapeshifts

Goal: a player in a vanilla beast form appears to peers as that beast, and as
the character again once back, with no transition animation. Mod-added
transformations are out of scope: peers keep showing the player's own look.

| Form | Race | Behavior graph |
| --- | --- | --- |
| werewolf | `Skyrim.esm` `0CDD84` `WerewolfBeastRace` | `Actors\WerewolfBeast\WerewolfBeastProject.hkx` |
| vampire lord | `Dawnguard.esm` `00283A` `DLC1VampireBeastRace` | `Actors\VampireLord\VampireLord.hkx` |
| werebear | `Dragonborn.esm` `01E17B` `DLC2WerebearBeastRace` | the werewolf's |

Vanilla gives the player no way to become a werebear, so only a mod triggers
it; its race is still the vanilla one. All three masters are base-game
plugins, so every receiver resolves them.

- **Source:** `Forms::ResolveBeastForms` resolves the races at `kDataLoaded`;
  `ShapeshiftSync::ObserveLocalForm` compares `Actor::GetRace` with them each
  pass, and the context's `shapeshift` carries the form
  ([Network](network.md#checkpoint-and-context)). A race switch rebuilds the
  player's graph, advancing the graph epoch. The equipment capture keeps the
  character's items and adds a vampire lord's spell hands and armor
  ([Equipment](#equipment)). Appearance capture pauses while transformed once a
  revision is published (`SyncAppearance`'s `a_transformed`), since a beast's
  `originalRace` is the player record's race and would respawn the beast on
  every peer. A game loaded in a beast form publishes its first revision with
  that race; the swap back publishes the right one.
- **Beast base:** `AppearanceSync::GetSpawnBase` builds, per form and
  appearance revision, a base like the [actor base](#actor-base) with the
  beast race and the race's attack data. The beast races have no head data,
  so it takes only the label, sex, weight, and height; the sex picks the
  vampire lord's model. Hair, skin color, and face tint presentation never runs
  for it (`ApplyActorPresentation` serves only the human base). A race that
  does not resolve logs once and shows the player's own look (`failedForm`).
- **Swap:** a played checkpoint whose form differs from the spawned one
  retires the actor (`AppearanceSync::RequiresShapeshift`,
  `ActorLifecycle::RetireForShapeshift`), at least 1 s after the previous
  spawn (`kMinimumShapeshiftIntervalSeconds`), and it spawns again from the
  played checkpoint in the new form, as a [respawn](#spawn-and-respawn) does.
  An appearance respawn takes the current form too. `RemoteActor::form`
  records the form the actor was built in.
- **Equipment:** a beast actor counts its hands as settled and runs no human
  armor, weapon, or inventory reconciliation: its runtime base has no
  inventory, and the werewolf and werebear wear only their skins. The
  vampire lord's transformation (`DLC1PlayerVampireChangeScript`) switches
  the race, then unequips everything and equips `Dawnguard.esm` `011A84`
  `DLC1ClothesVampireLordArmor` (the robe, spine, and loincloth over the
  skin); turning back removes it before the race switch, so the armor is
  only ever worn as the vampire lord. A vampire lord actor holds its
  source's spells and wears its armor (`EquipmentSync::SyncBeastEquipment`):
  - Each `beast_armor` entry resolves to its armor, or, when it does not
    resolve, to that vanilla armor (`Forms::GetVampireLordArmor`). The
    actor's inventory holds exactly one of each, worn; anything else is
    removed. The match is read from the inventory changes without
    allocating, every 5 s once matched.
  - Each `beast_*_hand` resolves to a
    [spell stand-in](combat-presentation.md#spell-look) as a human spell
    hand does, and a hand whose equipped spell differs gets `EquipSpell`, or
    `UnequipSpell` when it should be empty. Only spells are compared and
    swapped, so the race's own hand objects stay. A swap waits while an
    attack, a weapon transition, or a cast request owns the graph, at most
    2 s; armor never waits, as it does not touch the graph.
  - Checks run every 0.25 s, at once after a new revision or spawn, and at
    most 4 passes run before both match.

  The human actor reconciles from the character's items, which the revision
  kept, after the swap back.
- **Presentation:** locomotion drives the beast graphs
  ([Locomotion](locomotion.md#beast-graphs)); beast actions, levitation, and
  vampire lord casts are in
  [Combat presentation](combat-presentation.md#beast-actions); a beast never
  trades ([Trade](trade.md#invitation)).

### Display name

A player is known to others by `[Connection] DisplayName`
([Configuration](configuration.md#connection)), the same for every character
and save; the character's own name is not read or sent. There is no default,
and the plugin does not connect without one, so no peer is shown under a
placeholder.

The display name belongs to the player's session, unique per server
([Network](network.md#sessions)). The hello carries it, and the server writes
it into the `name` of every appearance it relays. It holds for the connection;
a new one takes effect with the next hello, so peers see the player leave and
enter under it. A replayed player keeps the name it was recorded with.

### Player label

`PlayerLabel::Format` builds the label for the actor base's name, chat
senders, and [nameplates](ui-overlay.md#nameplates): the display name, or a
placeholder before a peer's appearance arrives. The level is not part of it.

## Equipment

The actor wears and wields the source's armor, clothing, jewelry, shield,
weapons in either hand, torch, spell hands, and ammunition, and follows a change
within a fraction of a second, with no respawn, equip animation, or sound. Its
inventory holds only the synchronized equipped items: the rest of the source's
inventory is never sent, so the actor cannot equip anything else.

### Capture

Triggers:

- A `TESEquipEvent` sink for the player only sets an atomic dirty flag, since
  it runs on whichever thread fires the event.
- The pass captures once the flag has been quiet for 100 ms, coalescing
  multi-event changes: a favorites swap, a full outfit change, or a two-handed
  weapon clearing both hands.
- A full capture runs once per second anyway, for changes without an event
  (game load, script equips, race change).
- A captured attack, a starting hand presentation, or a starting shout
  captures at once and, if changed, publishes immediately, bypassing the
  quiet window and the minimum interval, so the new weapon or shout arrives
  before its use.

Capture, when the race has a [playable race](#playable-race):

- `armor`: every worn inventory entry (`ExtraWorn` or `ExtraWornLeft`) whose
  object is a `TESObjectARMO`, including shields, deduplicated after
  enchantment reduction. Non-playable armor is included, such as the Greybeard
  robes or the pieces of an outfit set on the player, except effect armor: a
  non-playable piece whose only slot is 61 (`BipedObjectSlot::kFX01`), as
  Dragon Aspect's `DLC2DragonbornAspectArmor01`–`03`, which the receiver's own
  copy of the effect equips ([Reconciliation](#reconciliation)). Skin armor
  never appears in inventory.
- Hands from `Actor::GetEquippedObject(false/true)`: a `TESObjectWEAP` other
  than hand-to-hand is `weapon` with its type, and a staff that casts also
  carries the spell it casts and that spell's look
  ([Combat presentation](combat-presentation.md#staves)); a carriable
  `TESObjectLIGH` in the left hand is `torch`; a `SpellItem` or `ScrollItem` is a `SPELL` hand with
  the spell and its look ([Combat presentation](combat-presentation.md#spell-look));
  bare fists or anything else is an unset hand. A two-handed
  weapon, bow, or crossbow is reported in the right hand only. Dual-wielding one
  weapon sends it in both hands.
- `ammo`: the worn `TESAmmo` entry. `Actor::GetCurrentAmmo` (vfunc 0x9E) reads
  `MiddleHighProcessData::bothHands` instead, which an actor without
  middle-high process data does not have.
- `shout`: the voice slot (`Actor::selectedPower`) when it holds a
  `TESShout`; a power is sent when cast
  ([Combat presentation](combat-presentation.md#shouts-and-powers)).
- `defaultOutfit`: the pieces for items that are not from base-game plugins
  ([Default look](#default-look)); empty with `AlwaysUse`, whose pieces are
  `armor`, `hands`, and `ammo` themselves.
- `beast_right_hand`, `beast_left_hand`, `beast_armor`: unset or empty.

Without a [playable race](#playable-race), as in a transformation, which
unequips everything, the last published items stand; outside the vanilla beast
forms the sender logs `Equipment sync not capturing in a non-playable race` once
per race. As a vampire lord, whose transformation equips its spells
and armor, a `SpellItem` in either hand also becomes that `beast_*_hand`,
with its look, as for a human spell hand; anything else in a hand is unset.
Every worn armor entry, effect armor included, becomes `beast_armor` by the
same rules as `armor`, at most 8, so a mod that changes the vampire lord's armor
shows it to peers who have that mod. Other beast races capture nothing. The human fields thus stay the character's, a human
actor never takes up the vampire lord's spells, and a late joiner gets both.

A player-enchanted item keeps its base form and carries the enchantment as
`ExtraEnchantment`, so only the unenchanted form is sent. Vanilla pre-enchanted
records point at their unenchanted item through `templateWeapon` or
`templateArmor`; the sender follows that chain, at most 8 steps, and skips the
item if the root is dynamic. Crafted, unenchanted items are ordinary forms.
Vanilla staves keep their model in the root, and a staff's enchantment, which
is what it casts, travels beside it in the hand.

Publishing: when the capture differs from the last published one (revision
excluded, armor compared as a set), at least 250 ms apart, sending only the
newest capture when the interval ends. Each publish increments `revision`, and
the last one is republished on every new connection. The sender publishes every
referenceable item, vanilla or not; all filtering is on the receiver.

### Resolution

An entry is accepted if `TESDataHandler::LookupForm` resolves it to the expected
type: `TESObjectARMO`, `TESObjectWEAP`, a carriable `TESObjectLIGH`, or
`TESAmmo`. Any loaded plugin qualifies, at any local load-order index. A
reference names the defining plugin, so a form overridden by a local mod still
resolves, as the receiver's version. A spell hand resolves to the receiver's
own version of a hand spell, or otherwise to a stand-in spell
([Combat presentation](combat-presentation.md#spell-look)). A hand whose
resolved weapon, or default outfit stand-in, is a staff also resolves the
staff's enchantment that way, when one or its look was sent
([Combat presentation](combat-presentation.md#staves)); its drops are logged
with the hand's slot followed by `Staff`. Each drop is
logged once per revision with player, plugin, form ID, reason (`missing`,
`wrong-type`, and for a spell `unsent` and `stand-in-failed`), and the
stand-in; a spell drop also names the look it fell back to (`look`,
`placeholder`). The filter is one
policy function returning a form or a drop reason, whose drop branch selects
the default outfit. A resolved or outfit torch is then replaced by its
[fake light](#fake-lights) version. A shout resolves to a `TESShout` with a
spell in at least one variation (else `no-spell`) and otherwise falls back to
Unrelenting Force; it is not equipped, only cast
([Combat presentation](combat-presentation.md#shout-and-power-receiver)).

### Fake lights

Goal: a remote actor's light never takes one of the engine's few
shadow-casting light slots, while looking the same. `Skyrim.esm` has a
shadow-casting `Torch01Shadow` (`00036343`) beside the plain `Torch01`
(`0001D4EC`), and mods may flag torches too.

`FakeLight::Get` returns a light without `kSpotShadow`, `kHemiShadow`, and
`kOmniShadow` as is; for one with any, a runtime copy (`DynamicForm::Create`,
game thread) with the same name, model and texture swaps, icons, weight,
value, destruction data, equip slot, bounds, light data, fade, sound,
emittance, and lens flare, without the shadow flags. Copies are cached by the
source's form ID, shared, never deleted, and logged once as `Fake light
created`. It applies to a remote torch in `ResolveHandEntry` (own or outfit)
and to every spell stand-in's hand light
([Combat presentation](combat-presentation.md#spell-look)), not to a spell the
actor holds as its own. The [light budget](remote-actor.md#light-budget) still
counts them.

### Reconciliation

The actor's inventory must always equal the resolved set. The runtime base has
no container or outfits, and reconciliation is the authority for everything
else. It runs for a ready, 3D-loaded actor and compares live inventory counts,
worn armor, and equipped hand objects with the resolved set. A worn shield,
which `GetEquippedObject(true)` reports as the left hand's object, counts as an
empty left hand and is compared with the armor. When they differ:

1. Required counts are one per armor entry and one per hand object (two when
   dual-wielding one weapon), plus 20 of the resolved ammo.
2. Every other object, and any excess, is removed and deleted, never
   transferred to a container or the player: gold, leveled items, script items.
   Worn non-playable armor that no revision from this peer resolved is kept
   at its count and is not compared with the worn set: it comes from the
   actor's own magic, such as `Dragonborn.esm` `DLC2DragonbornAspectArmor01`,
   which Dragon Aspect's script equips and unequips when the effect ends.
   Once unworn it is removed as any other object.
   `RemoteState::sentNonPlayableArmor` remembers every non-playable piece a
   revision resolved, for the peer's lifetime, so a piece the source takes off
   is removed rather than kept as effect armor.
3. Equipped armor and hand objects outside the set are unequipped. A spell in
   a hand that should be empty is removed with
   `ActorEquipManager::UnequipSpell` (ID 38902, SE 37946, Papyrus
   `Actor.UnequipSpell`), whose `ActionForceEquip` lowers a drawn hand;
   `Actor::DeselectSpell` sent no graph event and left the hand raised.
4. Missing objects are added.
5. Armor, then the right hand, then the left, are equipped with
   `ActorEquipManager::EquipObject` in the matching `BGSEquipSlot` (either for
   two-handed), `queueEquip=false`, `forceEquip=true`, `playSounds=false`. A
   spell hand uses `EquipSpell` in its slot, which also replaces a held spell
   with another, and never counts as inventory. The
   ammo is equipped, which fills the quiver; remote shots never spend it.
6. Each hand that holds no spell gets its selection
   (`Actor::selectedSpells`) through `Actor::SelectSpell` (ID 38768, SE
   37819): a staff's resolved spell, and otherwise none, which also clears
   what a staff left. A weapon's own enchantment, which never casts, may stay
   selected. A spell hand's selection is `EquipSpell`'s.

A mismatch is checked every frame until the first mutation pass, so a new
revision applies at once; afterwards, and while matching, every 0.25 s. At most
4 mutation passes run before the state matches; then the actor keeps what it
reached until a new revision, a respawn, or a later match resets the budget.
While it keeps it, the hands and worn armor are compared without allocating
every 0.25 s, and the inventory is read in full only when they match or every
5 s, which is how a later match is noticed.
Checking the live state also undoes the actor's own AI auto-equip, outfit
re-application, and unequip-on-hit.

Hand timing:

- Armor applies immediately in every graph state. Hand and ammo swaps are
  instant on the live actor, even while drawn, but wait while a draw or sheathe,
  an attack, a bow draw or shot, or a cast owns the graph, bounded to 2 s.
- `handsMatch` requires the equipped ammo and the hands' selections too, so a
  bow draw nocks the right arrow and a staff casts its own spell.
  `AreHandsSettled` gates attack dispatch and hand presentation, and
  `IsReadyForWeaponDraw` holds a first draw, at most 1 s.
- Retiring an actor keeps the equipment state; the new reference reconciles
  from it. Equipment never triggers a respawn.

### Default look

Each player configures a default look, a race and an outfit of armor, weapons,
and ammunition, that peers show in place of the player's items and race they
cannot resolve; the receiver has no look of its own. Its forms are limited to base-game plugins
(`Multiplayer::IsBaseGamePlugin`): the five Special Edition masters and
`ccFSVSSE001-Backpacks.esl`, which only uses slot 47, so a receiver without it
shows no backpack and everything else. Other Creation Club plugins are not
base-game, since a receiver without the Anniversary upgrade lacks them; a worn
Creation Club item gets a stand-in like any modded item.

`DefaultLook` resolves every configured form at `kDataLoaded` and whenever
the settings change `[DefaultLook]` (`Refresh`, first in each pass), checked
with the key's item filter. A form that is missing or does not fit is logged
with its key (`Default look item ignored ... reason=does-not-fit`) and
replaced by the key's default, or treated as unset when the default is unusable
too, such as the Anniversary backpack on an install without it. An empty
`Head`, `Circlet`, or `Backpack` is unset by choice and resolves to no piece
without a warning ([Configuration](configuration.md#defaultlook)).

Each equipment capture selects pieces only for items not from base-game
plugins (for every item with [`AlwaysUse`](#always-use)):

- `armor`: the piece of each outfit slot a modded worn item covers, in the
  order of `DefaultLookSettings::kArmorSlotPrecedence`: Body, Head (for Head
  or Hair), Hands, Feet, Shield, Amulet, Circlet, Backpack. Each piece carries
  the slot mask of the outfit slot it stands in for.
- `hands`: the weapon of each modded hand weapon's type and `Torch01` for a
  modded torch, each at most once. The torch is not configurable, since it is
  the only carriable base-game torch besides test items.
- `ammo`: the `Bolt` or `Arrow` piece for modded bolts or arrows.

An all-base-game player sends an empty outfit. A changed outfit publishes a
new equipment revision, a changed default race or preset face a new appearance
revision (every receiver then respawns the player once).

#### Preset face

`[DefaultLook] Preset` names a RaceMenu preset (`.jslot`) in
`Data\SKSE\Plugins\CharGen\Presets`, whose face the default look shows on
`[DefaultLook] Race` in place of the race's default face, both
with [`AlwaysUse`](#always-use) and on
a receiver that falls back to `defaultRace`. RaceMenu itself is not used: its
menu edits only the player (Skyrim has no looks menu for other actors), and
putting the default look on the player would change the race and every SKEE
layer. A preset is a file SKEE applies to any actor, so reading it leaves the
game untouched.

`LookPreset::Parse` reads the file ([format](#engine-assumptions)) into a
`PresetFace`: head parts by plugin and local form ID, hair color, face details,
the 19 face morphs, the 4 face presets (255 or `0xFFFFFFFF` is unset, -1), and
the tint layers with a non-zero alpha. A file that is not JSON, whose
signature is not SKEE's, or whose `formatVersion` is 0 is unreadable. Custom
slider and sculpt morphs, transforms, overrides, skin overrides, and body
morphs only set `raceMenuData`.

`DefaultLook` resolves it on the game thread, with the other forms:

- Race: `[DefaultLook] Race`. The preset stores none, since SKEE applies it
  to the target's own race, and the dropdown stays the one place that sets the
  look's race.
- Sex: the preset stores none, since SKEE applies it to the target's own sex.
  The face part's `kMale`/`kFemale` flag decides, else the majority of the
  other parts flagged for one sex, else the player's sex at resolve time
  (logged).
  So the default look can have another sex than the character.
- Head parts: extra parts are skipped, since their parent lists them. A part
  that is missing, not from a base-game plugin, or of a type above 6 counts as
  modded; one whose valid races reject the race, or that is flagged only for
  the other sex, as unfit. Either is replaced by the race's default part of its
  type for the sex when it is base-game. Of two parts of one type the first is
  kept, so at most one part of each type 0..6 remains.
- Face details: kept when base-game and a texture set, else counted as modded.
- Tints: each layer is matched to the race's tint asset for the sex by its
  texture path, case-insensitively, which gives the index and type. A layer
  matching none (an add-on war paint) or past 16 is dropped. The skin tone
  layer's color is the body tint, else the race's default skin tone.
- Weight, height, and level stay the character's.

The applied preset's result is logged once (`Default look preset loaded:
preset=... race= female= headParts= modded= wrongRace= wrongSex=
duplicate= replaced= ... tints= droppedTints=`); an unreadable preset, or a
race that does not resolve, leaves the default look with the race's face.
For the [Default Look tab](ui-overlay.md#settings-window),
`RequestPresetOutcome` asks for a staged preset and race, which `ServeRequests`
resolves the same way without logging on the next main-loop pass, paused
included. `GetPresetOutcome` returns the latest outcome (unreadable, and the
counts), keyed by preset and race.

#### Always use

`[DefaultLook] AlwaysUse` makes the sender publish the outfit as its real
look, so every receiver shows it whatever mods it has, and a receiver needs no
knowledge of the setting. The selection above runs for every worn item,
base-game or not, and the capture then keeps only its result:

- Appearance: `DefaultLook::GetAlwaysUseRace` gives the default look's race and
  `CaptureLocalAppearance` publishes, with the player's level, weight, and
  height, the [preset face](#preset-face) for it in the face's sex
  (`MakeFaceAppearance`), or without a preset `MakeDefaultAppearance` in the
  player's sex: the look a receiver builds for a race it cannot resolve.
  Head parts, tints, hair color, face data, and RaceMenu overlays are not the
  character's. `defaultRace` and `defaultFace` are unset, since `race` is
  already base-game.
- Equipment: `armor` holds only the outfit pieces, each with its resolved
  item's own slot mask, so an item covering only slots without a piece, or
  whose slot is set to none, is hidden. Each weapon and torch hand takes the
  outfit piece of its weapon type (a hand without one is dropped) and `ammo`
  the outfit arrow or bolt. Spell hands, a staff's enchantment, the shout, and
  the beast fields are left as captured, since they are what the player casts,
  not what is worn.
- The wire, the receiver, and the server see an ordinary character in base-game
  gear.

The receiver treats the outfit as part of the equipment revision:

1. The union of the unresolved armor entries' slot masks becomes the missing
   slots. A piece is a candidate when its stand-in mask covers a missing slot
   and it resolves to armor with slots.
2. `DefaultLook::SelectArmor` takes the resolved armor and the candidate
   pieces by rank: the position in `kArmorSlotPrecedence` of their main slot,
   the resolved item's own slots or the piece's stand-in slots. A resolved item
   comes before a piece of the same rank. A piece is skipped once every missing
   slot it stands in for is taken, and any item whose slots overlap one already
   taken is left out, since equipping it would unequip that item. So a missing
   cuirass with built-in gauntlets gets body and hands pieces except where real
   gauntlets show, a missing helmet or hood, on Head, Hair, or both, gets the
   one Head piece, and a hooded Body piece wins over the Head piece and over a
   resolved helmet rather than leave the body bare. A resolved item left out is
   logged as `left out resolved armor overlapping a higher slot`.
3. An unresolved weapon uses the outfit weapon of its `weaponType`, so attacks
   keep their animations; a two-handed stand-in still clears the left hand. An
   unresolved torch uses the outfit torch, and unresolved ammo the outfit ammo.
4. Without a piece, or when the piece does not resolve to the expected type,
   nothing is shown there. Each unresolved entry is logged once per revision with
   its stand-ins as `standIn`, or `none`.

## Item filter and catalog

`Forms::ItemTraits` decides whether an item can stand in for another: its kind
(armor, weapon, ammo, or a playable race as `kRace`), slot mask, weapon
type, bolt flag, and whether a base-game plugin defines it.
`Forms::GetItemTraits` reads them from a form and gives none for non-playable
armor (`TESObjectARMO::RecordFlags::kNonPlayable`), weapons, and ammunition
(`GetPlayable`): these are NPC-only variants that can show a player wrongly,
such as without a head, and `Forms::ItemFilter::Matches`
checks them against what a setting accepts. `GetLookArmorFilter`,
`GetLookWeaponFilter`, and `Config::kLookArrowFilter`, `kLookBoltFilter`,
`kLookRaceFilter` build every `[DefaultLook]` filter.
The settings window lists only matching items and `DefaultLook` resolves with
the same filters, so the list and the live behavior never disagree.

| Key | Accepts (base-game only) |
| --- | --- |
| `<Slot>` | armor whose main slot it is: armor covering the slot and no slot earlier in `kArmorSlotPrecedence`, so each item fits one key and a hooded robe is a `Body` piece, not a `Head` one |
| `Head` | the same, with Head or Hair as the slot; vanilla helmets cover Hair and Circlet, so they fit `Head` and only circlet-only items fit `Circlet` |
| weapon keys | a weapon of exactly that type |
| `Arrow`, `Bolt` | ammunition that is not, or is, a bolt |
| `Race` | a playable race |

`Forms::BuildItemCatalog` runs at `kDataLoaded`, before `DefaultLook` resolves
anything, and collects every named `TESObjectARMO`, `TESObjectWEAP`,
`TESObjectLIGH`, `TESAmmo`, and `TESRace` with traits and a plugin-local
reference, leaving out enchanted variants, which look like their listed
template. It is sorted by name, compares plugin names without case, never
changes, and is published through an `std::atomic<std::shared_ptr>`, so the
overlay reads it without the engine. `Search` keeps items matching a filter
whose lowercase name or plugin contains every query word; it runs when a list
opens and when the query changes, never per frame. `IsPluginLoaded` tells a
missing plugin apart from an unlisted item.
`Item catalog built: items=N plugins=N` logs once.

## Engine assumptions

For a dynamic form without a preprocessed FaceGen file, Skyrim builds the head
at runtime from the base's head parts and face data.

1. `CreateReferenceAtLocation` completes 3D and graph initialization for a base
   built from engine defaults, and the firewall, combat suppression, and
   furniture entry behave as with a base copied from the player's.
2. `TintAsset::texture.index` matches `TESNPC::Layer::tintIndex`, and
   `interpolationValue = alpha × 100` with the matching preset reproduces war
   paint and face tint colors exactly.
3. `TESEquipEvent` fires for favorites swaps, the inventory menu, hotkeys, and
   script `EquipItem`/`UnequipItem`; otherwise the 1 s poll covers it.
4. `GetEquippedObject(true)` may also report two-handed weapons, bows, and
   crossbows in the left hand; capture handles either answer. It reports a
   worn shield's `TESObjectARMO` there too.
5. `EquipObject` with `forceEquip=true` swaps a drawn weapon in place and
   updates `iRightHandType`/`iLeftHandType` without events the firewall blocks.
6. Actors do not auto-equip or unequip once their inventory holds only the
   synchronized set; removing items without a destination deletes them.
7. Every vanilla enchanted item's template chain ends at a non-dynamic,
   unenchanted form.
8. `EquipObject` with `queueEquip=false` updates `ExtraWorn` and the hand
   objects before the next check, 0.25 s later; twenty equipped arrows show in
   the quiver.
9. [Fake lights](#fake-lights), from the 1.7.104 executable:
   `TESObjectLIGH::GenDynamic` (ID 17610) creates a shadow light exactly when
   the form's flags (+0xEC) have a bit of `0x1C00`. The light's `Copy` is
   TESForm's empty one (ID 14657); each component's `CopyComponent`
   (`TESFullName` 14716, `TESModelTextureSwap` 14829, `TESIcon` 14957,
   `BGSMessageIcon` 14278, `TESWeightForm` 14987, `TESValueForm` 14977,
   `BGSDestructibleObjectForm` 14144, `BGSEquipType` 14217) copies its data. A
   factory-built copy equips as a torch with the same flame, light, and
   flicker, and a spell's hand light accepts it.
10. Face presets, from the 1.7.104 executable (SE not checked): `parts` default
    to `-1` (ID 24689); outside `RaceSexMenu` (ID 52402), which edits only the
    player's base, they only format a morph name (IDs 24713, 24714, 24766:
    `NoseType`, `BrowType`, `EyesType`, or `LipType` plus the number), so an
    unknown preset names a missing morph rather than indexing a table. The race
    preset bitmasks (`TESRace` +0x4A8, ID 25378) hold 256 bits per type.
11. [Shapeshifts](#shapeshifts): `CreateReferenceAtLocation` completes 3D,
    controller, and graph for a beast-race runtime base without head data, the
    race's skin dresses it, a female base takes the vampire lord's female
    model, and the beast races' abilities stay behind the magic firewall.
    `Actor::GetRace` on the player returns the beast race from the pass the
    switch takes effect and the original race from the pass it ends, and the
    switch replaces the graph manager.
12. Head parts, from the 1.7.104 executable: the head build (ID 25366)
    attaches exactly the base's `headParts` (ID 26836 per part, without extras)
    when `numHeadParts` is non-zero, otherwise the race's defaults with extras
    (ID 24730). `TESNPC::InitItemImpl` (ID 24719) replaces a plugin NPC's list
    with the expanded one: own parts, then the race's defaults, each followed
    by its extras (visitor ID 24856, which skips a type in `1..4` or `6` whose
    mask bit is set, ID 23632). No head part code indexes a table by
    `BGSHeadPart::type`: it is compared with fixed vanilla types or another
    part's, or used as a wrapping mask bit. The 1.5.97 type check is
    byte-identical; its visitor and head build were not traced.
13. [Skeleton scale](#capture), from RaceMenu's scripts and the vanilla
    skeleton: the height slider (`RaceMenuPlugin.pex`) sets an SKEE node
    transform scale on the `NPC` node (key `RSMPlugin`), written into its local
    scale. In `skeleton.nif` that node is a child of the root with scale 1,
    holds `NPC Root [Root]`, and is not animated; the engine scales the root by
    the reference scale and heights, so the node's scale is RaceMenu's alone.
    A beast skeleton is not checked: the beast base takes the published
    `height` either way.
14. [Tint render](#spawn-and-respawn), from both executables: the render
    (ID 27040, SE 26454) reads a mask's `texture` only for `textureName` (+0x8)
    and passes it to the texture load (ID 76005, SE 74273) before returning.
    From 1.7.104: the render queues a job that `Main::Update` (ID 36564) runs
    in queue order, all queued jobs in one pass (ID 27041) after the main-loop
    hook. Each job renders into the shared face tint render target and, when it
    has a destination, copies the target into a new texture (ID 77318). The
    player tint update (ID 52396, SKSE's `UpdatePlayerTints`, run by RaceMenu
    on tint edits) queues the player's list with no destination and gives the
    player's face a texture that views the target (ID 77302), so later jobs
    draw on the player's face until its head is rebuilt. The player's list is
    `overlayTintMasks` when set, else `tintMasks` (ID 40410). The SE side of
    this is not checked; the restore render is harmless either way.
15. From `RaceMenu.psc`: a war paint choice calls
    `Game.SetTintMaskTexturePath`, and a loaded game reapplies saved paths
    (`Game.SetNthTintMaskTexturePath`), add-on war paints too; SKSE writes the
    path into the mask's `TESTexture`, the race asset's, so the masks still
    match their asset by pointer. Overlay editing saves node overrides on
    `<Area> [Ovl#]` with the keys in [Overlays](#overlays), and `UpdateOverlay`
    reads them back the same way.
16. SKEE's `AddOverlays` installs overlays on a non-player reference whatever
    `[Overlays] bPlayerOnly` says, and reinstalls them with the node overrides
    when its 3D is rebuilt. SKEE's co-save entries for live remote actors name
    no reference on load, since they are left out of saves
    ([Saves](sync-pipeline.md#saves)).
17. A path `TextureExists` finds, with or without a leading `textures\`, loads
    the same file in the tint render and in SKEE.
18. The RaceMenu preset format, from SKEE's `PresetInterface` (`formatVersion`
    3) and a preset saved by RaceMenu Anniversary Edition: `version.signature`
    is `0x45534B53`; `headParts[].formIdentifier` is `Plugin|XXXXXX`, the form
    ID without the plugin's load order bits in hexadecimal, and `type` is the
    part's index in the player's head part list with extra parts left out, not
    `BGSHeadPart::type`; `actor.hairColor` is `0xRRGGBB`; `actor.headTexture`
    is the face details texture set's identifier; race and sex are not stored; `tintInfo[].color` is ARGB
    with the mask's alpha in the top byte and `index` the position in the
    player's tint mask list; `morphs.default.morphs` holds the 19 morphs and
    `morphs.default.presets` the 4 presets. `faceTextures` repeats the face
    part's texture set when nothing replaced it, so it does not count as
    RaceMenu data.

## Acceptance criteria

1. A remote actor shows the source's race, sex, weight, hair, eyes, hair color,
   face shape, skin and face tints, and its label, and wears the source's armor,
   clothing, jewelry, shield, hand items, and ammo from any plugin the receiver
   has, non-playable armor and outfits set on the player included. Late
   joiners see both without the source changing anything.
2. An appearance change (barber, RaceMenu) updates the actor within a few
   seconds with a single respawn, returning to its locomotion and furniture
   state; a level change does not respawn it or change its label. An
   equipment change applies within 0.5 s without respawn, animation, or sound.
3. The actor's inventory holds exactly the synchronized items, also after a cell
   reload, a respawn, and a hit reaction. The local player's appearance,
   inventory, and equipment never change because of a remote actor, also while
   the local player edits tints in RaceMenu and a remote face is built.
4. A peer missing a race or head part plugin sees a vanilla fallback, or the
   source's default race in the source's sex with the race's default look, or
   with the preset face in the preset's sex when the source configured one, and
   no crash.
5. Armor from a missing plugin shows the source's outfit armor in the covered
   slots without displacing resolved items of the same or a higher slot in
   `kArmorSlotPrecedence`, and a body piece is never left out for a head piece
   or a resolved helmet; a missing weapon, torch, or ammunition shows the
   outfit's of the same type, and attacks use that type's animations. Without a usable piece the item is left off.
6. Player-enchanted items appear as their unenchanted base item.
7. Drawing right after spawn or a swap draws the synchronized weapon, and the
   next attack uses its animation.
8. A remote player carrying `Torch01Shadow`, or a spell whose hand light casts
   shadows, shows the same torch or hand light, and the light casts no shadow
   and leaves the world's shadow-casting lights as they are.
9. A player's label shows the configured display name whatever the character is
   called, and whatever name its client put in the appearance. A display name
   changed while disconnected shows to peers from the next connection. Without
   a display name the player never appears to peers.
10. A player turning into a werewolf, vampire lord, or werebear appears to
    peers as that beast at once after the race switch, holding nothing but a
    vampire lord's spells and wearing nothing but its armor, with the same
    label; turning back restores the character's look and equipment
    with one more respawn. A late joiner sees the beast directly.
11. A female player's vampire lord uses the female model.
12. A mod's own transformation race leaves peers showing the player's own
    look.
13. A vampire player appears to peers in the character's own race, wearing and
    wielding what it has equipped.
14. A character made taller or shorter with RaceMenu's height slider or a
    reference scale appears to peers at the same height within `0.85..1.15`,
    and at the nearer bound beyond it, whether or not they have RaceMenu; a
    player without RaceMenu appears at the base's height.
15. A head part of any type that resolves on the receiver for the actor's race
    is shown on the remote actor with its extra parts, so peers with the same
    mods see the same hair, nails, and tails. A head part the receiver lacks,
    or whose valid races reject the resolved race, is left out without
    affecting the rest of the look.
16. A player with up to 32 top-level head parts publishes all of them.
17. A player wearing a RaceMenu alternative or add-on war paint appears to
    peers with it when they have its texture, and without that layer when they
    do not; vanilla war paint shows as usual.
18. A player's RaceMenu body, hand, feet, and face paint appear on peers who
    have RaceMenu and the textures, with the same color, transparency, and
    glow, also after a cell change and a respawn. A peer without RaceMenu or a
    texture sees the player without that overlay, and nothing else changes.
19. A player who picks a RaceMenu preset for the default look appears with its
    hair, eyes, brows, beard, scars, hair color, face sliders and presets, and
    vanilla tints, in the preset's sex, to every peer that shows the default
    look, whatever mods that peer has, as `[DefaultLook] Race`. A modded part,
    or one that does not fit that race or sex, shows as the race's default of
    that type, and the rest of the preset is still used.

## Known limitations

- Voice type (the actor speaks with its race's player voice, or the even-toned
  one of its sex), actor values, mod-added transformation races, and
  RaceMenu/NiOverride data other than overlays (body morphs, node transforms,
  scaled bones, armor and skin overrides) are not synchronized, except the
  `NPC` node scale, which travels in [`height`](#capture).
- An overlay carries only its diffuse texture: the other texture set slots
  RaceMenu can set (a `|`-separated path list, indices 1..8) and spell
  overlays are not shown. A receiver with fewer slots of an area
  (`skee64.ini` `iNumOverlays`) shows only the first overlays of it. A tint or
  overlay whose path is not printable ASCII is not sent.
- The transformation itself, its animation, effect shader, and sound, is not
  shown; the swap is a respawn, so the actor is briefly absent while the new
  form's 3D loads. Custom skin armor and far skins use the resolved race's.
- Enchantments, tempering, poison, charge, player-given names, and SMP/CBPC
  physics are not shown, except the spell a staff casts.
- An outfit piece stands in only for its slot and weapon type, not for the
  missing item's look. A default race without a preset shows its own default
  face.
- The preset face carries vanilla data only, since every receiver must render
  it from base-game forms: RaceMenu's custom sliders, sculpting, transforms,
  overrides, skin and face textures, and body morphs are left out, so a
  heavily sculpted face looks simpler to peers. Add-on war paints, modded hair
  and other modded parts are replaced or dropped. The preset is not edited in
  game; RaceMenu stays the editor, and a preset saved again under the same name
  is read when `[DefaultLook]` next changes.
- Lights built into a mesh (an `NiLight` in the model, not a `TESObjectLIGH`),
  such as some modded armor or weapons, keep their shadows.
