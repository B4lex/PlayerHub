# Appearance and Equipment

## Status

The face tint paths that differ by runtime have not run: the tint mask
offset on AE 1.6.x and the face tint render on SE 1.5.97 (ID 26454), which
rest on CommonLib and the 1.5.97 disassembly.

Head parts of mod-added types and the extra parts listed on the receiver
([Actor base](#actor-base)) have not run in game.

Tint textures that differ from the receiver's race asset and RaceMenu overlays
([Overlays](#overlays)) have not run in game.

## Scope

A remote actor looks like its source player's character, carries the player's
display name, and wears and wields what the source has
equipped, from any plugin the receiver also has. While the source is in a
vanilla beast form, the actor is that beast ([Shapeshifts](#shapeshifts)). What the receiver cannot
resolve falls back to vanilla data or to the source's own default outfit, so
players with different modpacks still see each other dressed and armed.

The wire format is in [Network](network.md#appearance-and-equipment). How the
draw and attacks wait for the hands is in [Combat
presentation](combat-presentation.md). Where the display name and the outfit are
configured is in [Configuration](configuration.md#settings).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/appearance/AppearanceSync` | Capture and publish; receive, build the actor base, respawn requests, hair, skin, face tint, and overlay presentation |
| `actor/RaceMenuOverlays` | SKEE's overlay and override interfaces: capture the player's overlays, apply them to a remote actor, and release them ([Overlays](#overlays)) |
| `forms/TextureFiles` | `TextureExists`: whether the receiver's loose files or archives hold a texture, cached per path |
| `sync/equipment/EquipmentSync` | Capture and publish; receive, resolve, reconcile the actor's inventory and a vampire lord actor's spells and armor; hand readiness |
| `sync/equipment/SpellStandIn` | Capture a spell hand's spell and look; resolve it to a runtime stand-in spell ([Combat presentation](combat-presentation.md#spell-look)) |
| `sync/equipment/FakeLight` | A shadow-casting light's runtime copy without its shadow flags |
| `sync/outfit/DefaultOutfit` | Resolve `[DefaultOutfit]` and select the pieces for a capture |
| `sync/PlayerLabel.h` | `PlayerLabel::Format` |
| `forms/RaceForms` | `IsPlayableRace`; `GetPlayableRace`, the playable race a capture sends for a race ([Playable race](#playable-race)) |
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
  one, `originalRace` when that is playable, otherwise the capture is skipped.
  The player's `originalRace` is the player record's template race, not the
  character's, so it comes last. While the player is in a
  vanilla beast form, the last published revision stands and the form
  travels apart ([Shapeshifts](#shapeshifts)).
- `level`: `Actor::GetLevel`, so a level-up publishes a new revision. Peers
  receive it and show it in the [player menu](ui-overlay.md#player-menu)
  only ([Player label](#player-label)). `name`
  is left empty: the server sets the [display name](#display-name).
- `defaultRace`: the configured `[DefaultOutfit]` race when `race` is not from
  a base-game plugin, else unset.
- `height`: the visual height multiplier, `TESNPC::height` × the reference
  scale (`refScale / 100`) × the local scale of the third-person skeleton's
  `NPC` node rounded to 0.01, clamped to `0.25..4`. RaceMenu's height slider
  scales that node ([skeleton scale](#engine-assumptions)); without RaceMenu it
  stays 1 and `height` is the base's, 1.0 on the vanilla player record. The
  race's height is left out: the receiver's resolved race applies its own.
  While the player has no 3D the last node scale stands. The remote plays the
  source's graph `Speed` unchanged ([graph speed](remote-actor.md#graph-speed)),
  so a scaled character's legs keep the pace the source sees.
- `female`, `weight`, `headParts[0..numHeadParts)`, the hair color
  form's RGB, `faceDetails`, and `faceData` morphs and parts. Extra parts are
  implied by their parent and skipped; the receiver lists them again
  ([Actor base](#actor-base)). A head part's `type` is `BGSHeadPart::type`
  (PNAM), a `uint32` a mod can set to anything, and is sent as it is
  ([wire split](network.md#appearance)): RaceMenu
  add-ons such as `Racemenu Handnails.esp` nails (41) and
  `InariTail_RM_2-3.esp` tails (40) lie outside the vanilla `0..6`
  ([head part types](#engine-assumptions)). At most 32 top-level parts are
  sent (`kMaximumHeadParts`); a capture over the limit logs one warning naming
  the first part left out, again only after a capture within it. Parts (presets)
  are clamped to `-1..255` (`ClampFacePart`), with `-1` for none; see
  [face presets](#engine-assumptions).
- `bodyTint`: `TESNPC::bodyTintColor`, or the skin tone mask's color if black.
- `tints`: `PlayerCharacter` runtime `tintMasks` in blend order, through
  CommonLib's `GetPlayerRuntimeData`: `0xB10` on SE, the offset `GetNumTints`
  reads there (ID 39614), and `0xB20` on 1.7.104, as `GetNumTints` (ID 40700)
  reads it. Each mask maps to the race's `TintAsset` for the sex by
  texture (pointer, then case-insensitive path) for its stable `index`.
  Zero-alpha masks and masks without an asset are skipped. `texture` is the
  mask's `TESTexture::textureName`. RaceMenu shows an alternative or add-on
  war paint by rewriting the path of the slot's race asset, which the mask
  points at ([tint textures](#engine-assumptions)), so the index alone would
  show the receiver's original texture for that slot. A mask whose path fails
  the [texture path](network.md#texture-paths) rule is skipped and logged once
  per path.
- `overlays`: `RaceMenuOverlays::Capture` ([Overlays](#overlays)).

A capture that differs from the last published one must repeat in two
consecutive captures, so dragging an editor slider does not stream updates.
A look that keeps changing for 10 s (`kUnsettledCaptureWarningSeconds`) is
never published; the sender logs one warning per such streak, naming the first
differing value of each changed field, so the mod changing it can be found.
A settled look that fails `IsValidAppearance` is dropped and never published;
the sender logs the first failing field and its value
(`DescribeInvalidAppearance`), once per distinct reason until a publish
succeeds.
Publishes are at least 3 s apart, each increments `revision`, and the last one
is republished on every new connection.

### Playable race

`Forms::GetPlayableRace` returns a race flagged playable as it is, and
otherwise its morph race (`TESRace::morphRace`, NAM8) when that one is
playable. Every vanilla vampire race (`NordRaceVampire` `088794`,
`KhajiitRaceVampire` `088845`, and the rest in `Skyrim.esm`, carried over by
`Dawnguard.esm` and `Update.esm`) is not playable and names its own race there,
so a vampire character is captured as that race, for both appearance and
equipment, and peers see the character's own race and items. The beast races
name no playable morph race (the werebear names `WerewolfBeastRace`), and only
one step is taken, so transformations stay unplayable. These records were read
from the masters.

### Actor base

Each accepted revision gets a new runtime `TESNPC`, created with the form
factory and a dynamic form ID (`TESDataHandler::GetNextID`), and built from
engine defaults rather than copied from a template record, so remote actors
depend on no particular base form. The constructor leaves no class, AI
packages, factions, inventory, or outfits, and points `faceData` at a shared
engine default that is replaced rather than written. The base then gets:

- the `kUnique` flag: a remote player is one character, not a generic NPC;
- the full name set to the player label, read by the crosshair and other name
  displays;
- race and original race, sex, weight, and height, with height clamped to
  `0.85..1.15` (`kMinimumRemoteHeight`, `kMaximumRemoteHeight`) so an extreme
  scale cannot make a remote actor a giant or a doll in another player's world;
  the wire keeps the full [`height`](#capture) range;
- the race's attack data map and the default combat style, which
  `TESNPC::InitItemImpl` fills only for NPCs loaded from plugins; weapon draws
  read the attack data map without a null check;
- the resolved head parts, each followed by its extra parts, hair color, face
  details, morphs, parts, body tint, and tint layers
  (`interpolationValue = alpha × 100`, with the matching preset). Each layer's
  texture is kept beside the base in `RemoteState::baseTintTextures`: the race
  asset's path when `texture` is empty or names the same file, else the sent
  path.

`TESNPC::InitItemImpl` lists a plugin NPC's extra parts after their parent, and
the head build attaches only the listed parts ([head
parts](#engine-assumptions)). A runtime base skips `InitItemImpl`, so
`ResolveHeadParts` lists the extras itself; otherwise every mesh a mod puts in
an extra part, such as a hairline or a hair's main mesh, would be missing. The list
follows the engine's rules (`HeadPartList`): a part of a type that holds one
part (face, eyes, hair, facial hair, brows) that is already listed is skipped
with its extras, a part already listed is not repeated, and the list stops at
127 parts (`TESNPC::numHeadParts` is an `int8_t`). The engine tracks the
types in a 32-bit mask indexed by `type` modulo 32, so a mod-added type can
collide with a vanilla one there; `HeadPartList` compares exact types.

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

A fallback race keeps the source's own sex and takes the race's default look for
it: default head parts and skin tone. Mods add races but not sexes, so the
outfit has no sex setting. The source's head parts, tints, morphs, presets, face details, and hair color belong
to its own race and sex and are dropped; display name, level, weight, and height
are kept. Each fallback is logged once per revision.
`AppearanceSync::GetRaceName` applies the race rule alone to name a player's
race in its [player menu](ui-overlay.md#player-menu), logging the fallback
each time.

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
  or with `bUseFaceGenPreprocessedHeads` off, the same on both runtimes, so a
  remote actor's face would render dark. The receiver
  finds the face geometry (named after the face part's editor ID), takes its
  FaceGen material's tint texture, and queues the engine's tint render
  (ID 27040, SE 26454) with masks from the base's layers: the layer's texture
  from `baseTintTextures`, layer color, and `interpolationValue / 100` alpha,
  at most 16 non-zero layers. The render reads only the mask texture's
  `textureName` ([tint render](#engine-assumptions)), so each mask points at a
  stack `MaskTexture` with `TESTexture`'s layout and the asset's vtable. It
  renders again whenever the head gets a new tint texture, such as after a 3D
  reset.
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

- **Source:** `Forms::ResolveBeastForms` resolves the races at `kDataLoaded`.
  `ShapeshiftSync::ObserveLocalForm` compares `Actor::GetRace` with them each
  pass, and the context's `shapeshift` carries the form
  ([Network](network.md#checkpoint-and-context)). A race switch rebuilds the
  player's graph, so the graph epoch advances with it
  ([assumption 14](#engine-assumptions)). The equipment capture keeps the
  character's items in a non-playable race and adds a vampire lord's spell
  hands and armor ([Equipment](#equipment)). The appearance capture does not run while the form is
  not `none` once a revision is published (`SyncAppearance`'s
  `a_transformed`): a beast race's `originalRace` is the player record's
  race (Nord for an Argonian player, seen in game), and every revision would
  respawn the beast on each peer. A game loaded in a beast form publishes its
  first revision with that race; the swap back publishes the right one.
- **Beast base:** `AppearanceSync::GetSpawnBase` builds, per form and
  appearance revision, a base like the [actor base](#actor-base) with the
  beast race and the race's attack data. The beast races have no head data,
  so it takes only the label, sex, weight, and height; the sex picks the
  vampire lord's model ([assumption 13](#engine-assumptions)). Hair, skin color, and face tint presentation never runs
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

A player is known to others by a display name chosen in the settings,
`[Connection] DisplayName`
([Configuration](configuration.md#connection)), the same for every character
and save: 3 to 15 Latin letters, digits, and underscores
([Network](network.md#sessions)). The character's own name is not read or sent.
There is no default: the player must enter a display name before the plugin
connects ([Network](network.md#connection)), so no peer is shown under a
placeholder.

The display name belongs to the player's session, and no two sessions of a
server share one ([Network](network.md#sessions)). The hello carries it, and the
server writes the session's display name into the `name` of the appearance it
relays, whatever the client sent; the capture leaves `name` empty. The display
name holds for the connection: the settings window locks it while a connection
is wanted or open, and another one takes effect with the next hello
([Network](network.md#connection)), so peers see the player leave and enter
under it. A replayed player keeps the name it was recorded with.

### Player label

`PlayerLabel::Format` builds the label used for the actor base's name, chat
senders, and [nameplates](ui-overlay.md#nameplates): the display name, or a
placeholder for an empty one, as before a peer's appearance arrives.

The level is not part of the label. It stays in `Appearance`
([Network](network.md#appearance)), captured, validated, relayed, and parsed,
and the [player menu](ui-overlay.md#player-menu) shows it.

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
- A captured attack or a starting hand presentation captures at once and, if
  changed, publishes immediately, bypassing the quiet window and the minimum
  interval, so the new weapon arrives before its use.

Capture, when the race has a [playable race](#playable-race):

- `armor`: every worn inventory entry (`ExtraWorn` or `ExtraWornLeft`) whose
  object is a playable `TESObjectARMO`, including shields, deduplicated after
  enchantment reduction. Skin armor never appears in inventory.
- Hands from `Actor::GetEquippedObject(false/true)`: a `TESObjectWEAP` other
  than hand-to-hand is `weapon` with its type; a carriable `TESObjectLIGH` in the
  left hand is `torch`; a `SpellItem` or `ScrollItem` is a `SPELL` hand with
  the spell and its look ([Combat presentation](combat-presentation.md#spell-look));
  bare fists or anything else is an unset hand. A two-handed
  weapon, bow, or crossbow is reported in the right hand only. Dual-wielding one
  weapon sends it in both hands.
- `ammo`: the worn `TESAmmo` entry. `Actor::GetCurrentAmmo` (vfunc 0x9E) reads
  `MiddleHighProcessData::bothHands` instead, which an actor without
  middle-high process data does not have.
- `defaultOutfit`: the pieces for items that are not from base-game plugins
  ([Default outfit](#default-outfit)).
- `beast_right_hand`, `beast_left_hand`, `beast_armor`: unset or empty.

Without a [playable race](#playable-race), as in a transformation, which
unequips everything, the last published items stand; outside the vanilla beast
forms the sender logs `Equipment sync not capturing in a non-playable race` once
per race. As a vampire lord, whose transformation equips its spells
and armor, a `SpellItem` in either hand also becomes that `beast_*_hand`,
with its look, as for a human spell hand; anything else in a hand is unset.
Every worn armor entry, playable or not, becomes `beast_armor` by the same
rules as `armor`, at most 8, so a mod that changes the vampire lord's armor
shows it to peers who have that mod. Other beast races capture nothing. The human fields thus stay the character's, a human
actor never takes up the vampire lord's spells, and a late joiner gets both.

A player-enchanted item keeps its base form and carries the enchantment as
`ExtraEnchantment`, so only the unenchanted form is sent. Vanilla pre-enchanted
records point at their unenchanted item through `templateWeapon` or
`templateArmor`; the sender follows that chain, at most 8 steps, and skips the
item if the root is dynamic. Crafted, unenchanted items are ordinary forms.

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
resolves, as the receiver's version. A spell hand resolves to a stand-in
spell ([Combat presentation](combat-presentation.md#spell-look)). Each drop is
logged once per revision with player, plugin, form ID, reason (`missing`,
`wrong-type`, and for a spell `unsent`), and the stand-in; a spell drop also
names the look it fell back to (`look`, `placeholder`). The filter is one
policy function returning a form or a drop reason, whose drop branch selects
the default outfit. A resolved or outfit torch is then replaced by its
[fake light](#fake-lights) version.

### Fake lights

Goal: a remote actor's light never takes one of the engine's few
shadow-casting light slots, so remote players cannot crowd out the world's
shadows, while it looks the same. Vanilla `Skyrim.esm` has a shadow-casting
torch, `Torch01Shadow` (`00036343`, Shadow Omni), beside the plain `Torch01`
(`0001D4EC`), with the same mesh; a player who picks it up, or a mod that
flags torches, would otherwise carry a shadow light into every peer's world.

`FakeLight::Get` returns a light without `kSpotShadow`, `kHemiShadow`,
and `kOmniShadow` as it is. For one with any of them it returns a runtime copy,
created on the game thread with `DynamicForm::Create`, that has the same name,
model and texture swaps, icons, weight, value, destruction data, equip slot,
bounds, light data (radius, color, flicker, falloff, FOV), fade, sound,
emittance, and lens flare, and none of the shadow flags, so the engine makes
it a fake light, the usual Skyrim name for a light that casts no shadow.
Copies are cached by the source's form ID, shared by every actor and peer,
never deleted, and logged once as `Fake light created`.

It applies to a remote torch in `ResolveHandEntry`, whether the source's own
or the outfit's, and to the hand light of every spell stand-in
([Combat presentation](combat-presentation.md#spell-look)). The scene light
budget still counts these lights ([Remote actor](remote-actor.md#light-budget)).

### Reconciliation

The actor's inventory must always equal the resolved set. The runtime base has
no container or outfits, and reconciliation is the authority for everything
else. It runs for a ready, 3D-loaded actor and compares live inventory counts,
worn armor, and equipped hand objects with the resolved set. When they differ:

1. Required counts are one per armor entry and one per hand object (two when
   dual-wielding one weapon), plus 20 of the resolved ammo.
2. Every other object, and any excess, is removed and deleted, never
   transferred to a container or the player: gold, leveled items, script items.
3. Equipped armor and hand objects outside the set are unequipped. A spell in
   a hand that should be empty is removed with
   `ActorEquipManager::UnequipSpell` (ID 38902, SE 37946, Papyrus
   `Actor.UnequipSpell`), whose `ActionForceEquip` lowers a drawn hand;
   `Actor::DeselectSpell` sent no graph event and left the hand raised.
4. Missing objects are added.
5. Armor, then the right hand, then the left, are equipped with
   `ActorEquipManager::EquipObject` in the matching `BGSEquipSlot` (either for
   two-handed), `queueEquip=false`, `forceEquip=true`, `playSounds=false`. A
   spell hand uses `EquipSpell` in its slot, which also replaces a stand-in
   with another, and never counts as inventory. The
   ammo is equipped, which fills the quiver; remote shots never spend it.

A mismatch is checked every frame until the first mutation pass, so a new
revision applies at once; afterwards, and while matching, every 0.25 s. At most
4 mutation passes run before the state matches; then the actor keeps what it
reached until a new revision, a respawn, or a later match resets the budget.
Checking the live state also undoes the actor's own AI auto-equip, outfit
re-application, and unequip-on-hit.

Hand timing:

- Armor applies immediately in every graph state. Hand and ammo swaps are
  instant on the live actor, even while drawn, but wait while a draw or sheathe,
  an attack, a bow draw or shot, or a cast owns the graph, bounded to 2 s.
- `handsMatch` requires the equipped ammo too, so a bow draw nocks the right
  arrow. `AreHandsSettled` gates attack dispatch and hand presentation, and
  `IsReadyForWeaponDraw` holds a first draw, at most 1 s.
- Retiring an actor keeps the equipment state; the new reference reconciles
  from it. Equipment never triggers a respawn.

### Default outfit

Each player configures a default outfit and race that peers show in place of
the player's items and race they cannot resolve. Its forms are limited to
base-game plugins (`Multiplayer::IsBaseGamePlugin`): the five Special Edition
masters, which every receiver has, so they are expected to resolve, and
`ccFSVSSE001-Backpacks.esl`. Other Creation Club plugins are not base-game,
since a Special Edition receiver, or an Anniversary Edition one without the
upgrade, lacks them: an outfit cannot use them, and a worn Creation Club item
gets a stand-in like any modded item. The backpacks are the exception because
they only use slot 47: a receiver without them shows no backpack and every
other slot as usual. It is the source's choice; the receiver has no outfit of
its own.

`DefaultOutfit` resolves every configured form at `kDataLoaded` and whenever
the settings change `[DefaultOutfit]` (`Refresh`, first in each pass), checked
with the key's item filter. A form that is missing or does not fit is logged
with its key (`Default outfit item ignored ... reason=does-not-fit`) and
replaced by the key's default, or treated as unset when the default is unusable
too, such as the Anniversary backpack on an install without it. An empty
`Head`, `Circlet`, or `Backpack` is unset by choice and resolves to no piece
without a warning ([Configuration](configuration.md#defaultoutfit)).

Each equipment capture selects pieces only for items not from base-game
plugins:

- `armor`: the piece of each outfit slot a modded worn item covers, in the
  order of `DefaultOutfitSettings::kArmorSlotPrecedence`: Body, Head (for Head
  or Hair), Hands, Feet, Shield, Amulet, Circlet, Backpack. Each piece carries
  the slot mask of the outfit slot it stands in for.
- `hands`: the weapon of each modded hand weapon's type and `Torch01` for a
  modded torch, each at most once. The torch is not configurable, since it is
  the only carriable base-game torch besides test items.
- `ammo`: the `Bolt` or `Arrow` piece for modded bolts or arrows.

An all-base-game player sends an empty outfit. A changed outfit or setting
changes the next capture and publishes a new revision, as does a changed default
race or sex in the next appearance capture (every receiver then respawns the
player once).

The receiver treats the outfit as part of the equipment revision:

1. The union of the unresolved armor entries' slot masks becomes the missing
   slots. A piece is a candidate when its stand-in mask covers a missing slot
   and it resolves to armor with slots.
2. `DefaultOutfit::SelectArmor` takes the resolved armor and the candidate
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
checks them against what a setting accepts. `GetOutfitArmorFilter`,
`GetOutfitWeaponFilter`, and `Config::kOutfitArrowFilter`, `kOutfitBoltFilter`,
`kOutfitRaceFilter` build every `[DefaultOutfit]` filter.
The settings window lists only matching items and `DefaultOutfit` resolves with
the same filters, so the list and the live behavior never disagree.

| Key | Accepts (base-game only) |
| --- | --- |
| `<Slot>` | armor whose main slot it is: armor covering the slot and no slot earlier in `kArmorSlotPrecedence`, so each item fits one key and a hooded robe is a `Body` piece, not a `Head` one |
| `Head` | the same, with Head or Hair as the slot; vanilla helmets cover Hair and Circlet, so they fit `Head` and only circlet-only items fit `Circlet` |
| weapon keys | a weapon of exactly that type |
| `Arrow`, `Bolt` | ammunition that is not, or is, a bolt |
| `Race` | a playable race |

`Forms::BuildItemCatalog` runs at `kDataLoaded`, before `DefaultOutfit` resolves
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

Verified in game:

1. For a dynamic form without a preprocessed FaceGen file, Skyrim builds the
   head at runtime from the base's head parts and face data.

Not confirmed in game:

2. `CreateReferenceAtLocation` completes 3D and graph initialization for a base
   built from engine defaults, and the firewall, combat suppression, and
   furniture entry behave as with a base copied from the player's.
3. `TintAsset::texture.index` matches `TESNPC::Layer::tintIndex`, and
   `interpolationValue = alpha × 100` with the matching preset reproduces war
   paint and face tint colors exactly.
4. `TESEquipEvent` fires for favorites swaps, the inventory menu, hotkeys, and
   script `EquipItem`/`UnequipItem`; otherwise the 1 s poll covers it.
5. `GetEquippedObject(true)` may also report two-handed weapons, bows, and
   crossbows in the left hand; capture handles either answer.
6. `EquipObject` with `forceEquip=true` swaps a drawn weapon in place and updates
   `iRightHandType`/`iLeftHandType` without events the firewall blocks.
7. Actors do not auto-equip or unequip once their inventory holds only the
   synchronized set; removing items without a destination deletes them silently.
8. Every vanilla enchanted item's template chain ends at a non-dynamic,
   unenchanted form.
9. `EquipObject` with `queueEquip=false` updates `ExtraWorn` and the equipped hand
   objects before the next check, 0.25 s later; twenty equipped arrows show in
   the quiver.

Verified against the 1.7.104 executable for [fake lights](#fake-lights):

10. `TESObjectLIGH::GenDynamic` (ID 17610) creates a shadow light exactly when
    the form's flags (+0xEC) have a bit of `0x1C00`. The light's `Copy` is
    TESForm's empty one (ID 14657). The `CopyComponent` of each of its
    components (`TESFullName` 14716, `TESModelTextureSwap` 14829, `TESIcon`
    14957, `BGSMessageIcon` 14278, `TESWeightForm` 14987, `TESValueForm` 14977,
    `BGSDestructibleObjectForm` 14144, `BGSEquipType` 14217) casts its source
    by RTTI and copies its data.

Verified against the 1.7.104 executable for face presets, which the receiver
also clamps to `-1..255` when parsing:

11. `TESNPC::FaceData::parts` default to `-1` when the engine allocates face
    data (ID 24689). Outside `RaceSexMenu` (ID 52402), which edits only the
    player's own base, the parts are read only to format a morph name
    (IDs 24713, 24714, 24766): `NoseType`, `BrowType`, `EyesType`, or
    `LipType` followed by the number, or a fixed name for `0`. An unknown
    preset therefore names a morph that is not found rather than indexing a
    table. The race preset bitmasks (`TESRace` +0x4A8, ID 25378) hold 256 bits
    per part type, so presets stop at 255. SE 1.5.97 was not checked.

Not confirmed in game:

12. A factory-built `TESObjectLIGH` with copied components and data equips as a
    torch, raises the torch arm, and shows the same flame, light, and flicker,
    and a spell's hand light accepts it.

Not confirmed in game for [shapeshifts](#shapeshifts):

13. `CreateReferenceAtLocation` completes 3D, character controller, and graph
    for a runtime base of a beast race without head data, the race's skin
    dresses it, a female base takes the vampire lord's female model, and the
    beast races' own abilities stay behind the magic firewall.
14. `Actor::GetRace` on the player returns the beast race from the pass the
    race switch takes effect, and the original race from the pass it ends;
    the switch replaces the player's graph manager, which advances the graph
    epoch ([Locomotion](locomotion.md#source-capture)).

Verified against the 1.7.104 executable for head parts, not confirmed in game:

17. The head build for an actor (ID 25366) attaches exactly the base's
    `headParts` (ID 26836 per part, without extra parts) when
    `numHeadParts` is non-zero, and otherwise the race's defaults with their
    extras (ID 24730). `TESNPC::InitItemImpl` (ID 24719) replaces a plugin
    NPC's list with the expanded one from ID 24730: its own parts, then the
    race's defaults, each part followed by its extras through the visitor at
    ID 24856. That visitor skips a part whose type is one of `1..4` and `6`
    (ID 23632) when the mask already has its bit.
18. No head part code in the TESNPC or head build ranges indexes a table by
    `BGSHeadPart::type`: it is compared with fixed vanilla types (face in
    `PrepareHeadPartForShaders`, ID 26838; hair) or with another part's type,
    or used as a 32-bit mask bit, where the shift wraps. A scan of both
    executables for a `type` load used as an index found none in this code.
    The 1.5.97 type check (ID 23632) is byte-identical; the 1.5.97 visitor and
    head build were not traced.

Read from RaceMenu's scripts and the vanilla skeleton for the
[skeleton scale](#capture), not confirmed in game:

15. RaceMenu's height slider (`RaceMenuPlugin.pex`) sets an SKEE node
    transform scale on the `NPC` node with the key `RSMPlugin`, and SKEE writes
    the combined transform into the node's local scale. In `skeleton.nif` that
    node is a child of the `BSFadeNode` root, with scale 1, and holds
    `NPC Root [Root]` and every animated bone; animation does not drive it.
    The engine scales the root by the reference scale and the base and race
    heights, so the node's scale is RaceMenu's factor alone.
16. A beast form's skeleton is not checked: the beast base takes the published
    `height`, node scale included, whether or not SKEE scales the source's
    beast skeleton.

Verified against the 1.7.104 and 1.5.97 executables for the
[tint render](#spawn-and-respawn):

19. The face tint render (ID 27040, SE 26454) reads a mask's `texture` only
    for `textureName` (+0x8) and passes it to the texture load (ID 76005,
    SE 74273) before it returns; the render job keeps the loaded texture, not
    the mask.

Read from RaceMenu's scripts (`RaceMenu.psc`) for
[tint textures](#capture) and [overlays](#overlays), not confirmed in game:

20. A war paint choice calls `Game.SetTintMaskTexturePath`
    (`OnTintTextureChange`), and a loaded game reapplies the saved paths
    (`LoadTints`, `Game.SetNthTintMaskTexturePath`); add-on war paints
    (`AddWarpaint`) go through the same call. SKSE writes the path into the
    mask's `TESTexture`, which is the race asset's, so the player's masks still
    match their asset by pointer.
21. Overlay editing saves node overrides on `<Area> [Ovl#]` with the keys in
    [Overlays](#overlays) (`OnOverlayTextureChange`, `OnOverlayColorChange`,
    `OnOverlayGlowColorChange`), and `UpdateOverlay` reads them back the same
    way.

Not confirmed in game:

22. SKEE's `AddOverlays` installs overlays on a non-player reference whatever
    `[Overlays] bPlayerOnly` says, and SKEE reinstalls them and reapplies its
    node overrides when that reference's 3D is rebuilt. SKEE's co-save entries
    for actors that are live when the game saves name no reference on load,
    since remote actors are left out of saves
    ([Saves](sync-pipeline.md#saves)).
23. A path `TextureExists` finds, with or without a leading `textures\`, loads
    the same file in the tint render and in SKEE.

## Acceptance criteria

1. A remote actor shows the source's race, sex, weight, hair, eyes, hair color,
   face shape, skin and face tints, and its label, and wears the source's armor,
   clothing, jewelry, shield, hand items, and ammo from any plugin the receiver
   has. Late joiners see both without the source changing anything.
2. An appearance change (barber, RaceMenu) updates the actor within a few
   seconds with a single respawn, returning to its locomotion and furniture
   state; a level change does not respawn it or change its label. An
   equipment change applies within 0.5 s without respawn, animation, or sound.
3. The actor's inventory holds exactly the synchronized items, also after a cell
   reload, a respawn, and a hit reaction. The local player's appearance,
   inventory, and equipment never change because of a remote actor.
4. A peer missing a race or head part plugin sees a vanilla fallback, or the
   source's default race in the source's sex with the race's default look, and
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
17. A 1.1.0 peer still shows such a player, with only the parts of types
    `0..6`.
18. A player wearing a RaceMenu alternative or add-on war paint appears to
    peers with it when they have its texture, and without that layer when they
    do not; vanilla war paint shows as before.
19. A player's RaceMenu body, hand, feet, and face paint appear on peers who
    have RaceMenu and the textures, with the same color, transparency, and
    glow, also after a cell change and a respawn. A peer without RaceMenu or a
    texture sees the player without that overlay, and nothing else changes.
20. A 1.1.0 peer shows such a player with the race's own tint textures and no
    overlays.

## Known limitations

- Voice type, actor values, mod-added transformation races, and
  RaceMenu/NiOverride data other than overlays (body morphs, node transforms,
  scaled bones, armor and skin overrides) are not synchronized, except the
  `NPC` node scale, which travels in [`height`](#capture).
- An overlay carries only its diffuse texture: the other texture set slots
  RaceMenu can set (a `|`-separated path list, indices 1..8) and spell
  overlays are not shown. A receiver with fewer slots of an area
  (`skee64.ini` `iNumOverlays`) shows only the first overlays of it. A tint or
  overlay whose path is not printable ASCII is not sent.
- The transformation itself, its animation, effect shader, and sound, is not
  shown: the user chose an instant swap. The swap waits for a respawn,
  so the actor is briefly absent while the new form's 3D loads. Custom skin armor and far skins use the resolved race's.
- Enchantments, tempering, poison, charge, player-given names, the equipped shout
  or power, and SMP/CBPC physics are not shown.
- An outfit piece stands in only for its slot and weapon type, not for the
  missing item's look. A default race shows its own default face.
- Tint capture is off on VR.
- Lights built into a mesh (an `NiLight` in the model, not a `TESObjectLIGH`),
  such as some modded armor or weapons, keep their shadows.
