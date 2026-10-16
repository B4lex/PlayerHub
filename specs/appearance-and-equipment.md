# Appearance and Equipment

## Status

- **Appearance:** run in game between two clients on Skyrim AE 1.7.104 with a
  base copied from the local player base. The current base, built from engine
  defaults, and the default race are not yet built on Windows or run in game.
  The face tint render on SE 1.5.97 (ID 26454, 2026-10-12; before that AE
  only) passed a Linux syntax check only and was not run on SE.
- **Equipment:** run in game, including an earlier receiver-configured fallback.
  `UnequipSpell` on SE 1.5.97 (ID 37946, 2026-10-12; before that `DeselectSpell`
  off AE) passed a Linux syntax check only, as did spell hands carrying the
  spell and its look (2026-10-15). The items under [Engine
  assumptions](#engine-assumptions) are not individually checked.
- **Fake lights (2026-10-16):** the shadow flag test in `GenDynamic`
  and the copy components were read in the 1.7.104 disassembly; Linux syntax
  check only. Not built on Windows or run in game, and SE 1.5.97 was not
  checked.
- **Default outfit:** the sender's outfit and the receiver's ranked armor
  selection (`SelectArmor`) are not yet built on Windows or run in game. The
  default form IDs were checked against `Skyrim.esm` and `Dawnguard.esm` records
  (type, slots, weapon type).
- **Item filter and catalog:** built on Windows; the catalog and picker run in
  game ([UI overlay](ui-overlay.md#item-picker)). The main-slot armor filter is
  checked only in a standalone Linux test of `ItemFilter`, not yet built on
  Windows or run in game.
- **Player labels:** level validation and chat fields covered by server unit
  tests; label rendering pending an in-game check. The display name, which comes
  from the player's session ([Network](network.md#sessions)), and the label
  without the level are not yet built on Windows or run in game, and
  `AppearanceSync.cpp` was not compiled after either change.

## Scope

A remote actor looks like its source player's character, carries the player's
display name, and wears and wields what the source has
equipped, from any plugin the receiver also has. What the receiver cannot
resolve falls back to vanilla data or to the source's own default outfit, so
players with different modpacks still see each other dressed and armed.

The wire format is in [Network](network.md#appearance-and-equipment). How the
draw and attacks wait for the hands is in [Combat
presentation](combat-presentation.md). Where the display name and the outfit are
configured is in [Configuration](configuration.md#settings).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/appearance/AppearanceSync` | Capture and publish; receive, build the actor base, respawn requests, hair, skin, and face tint presentation |
| `sync/equipment/EquipmentSync` | Capture and publish; receive, resolve, reconcile the actor's inventory; hand readiness |
| `sync/equipment/SpellStandIn` | Capture a spell hand's spell and look; resolve it to a runtime stand-in spell ([Combat presentation](combat-presentation.md#spell-look)) |
| `sync/equipment/FakeLight` | A shadow-casting light's runtime copy without its shadow flags |
| `sync/outfit/DefaultOutfit` | Resolve `[DefaultOutfit]` and select the pieces for a capture |
| `sync/PlayerLabel.h` | `PlayerLabel::Format` |
| `protocol/AppearanceState.h`, `EquipmentState.h`, `FormReference.h` | Plain data and validators shared by capture and the network parser |
| `forms/FormLookup` | Encode and resolve `{plugin, formId}` |
| `forms/DynamicForm.h` | `DynamicForm::Create`: a factory-built form with a dynamic form ID |
| `forms/ItemFilter.h`, `ItemCatalog`, `ItemForms` | Item traits, filters, and the searchable catalog |

## Appearance

The actor shows the source's race, sex, body weight, height, head parts (face,
eyes, hair, beard, brows, scars), hair color, face morph sliders and presets,
face detail texture, skin tone, and face tint layers (war paint, dirt, lip and
cheek color).

### Capture

On the game thread, once per second:

- `race`: the actor base race; if it is not playable, `originalRace` when that
  is playable, otherwise the capture is skipped (transformed). While
  transformed, peers keep the last playable appearance.
- `level`: `Actor::GetLevel`, so a level-up publishes a new revision. Peers
  receive it and show it nowhere yet ([Player label](#player-label)). `name`
  is left empty: the server sets the [display name](#display-name).
- `defaultRace`: the configured `[DefaultOutfit]` race and sex when `race` is
  not from a base-game plugin, else unset.
- `female`, `weight`, `height`, `headParts[0..numHeadParts)`, the hair color
  form's RGB, `faceDetails`, and `faceData` morphs and parts.
- `bodyTint`: `TESNPC::bodyTintColor`, or the skin tone mask's color if black.
- `tints`: `PlayerCharacter` runtime `tintMasks` in blend order. On 1.7.104 the
  array is at `0xB20` (CommonLibSSE-NG reads `0xB18`); it is used only when it
  matches `GetNumTints`/`GetTintMask` (IDs 40700/40698), which otherwise list
  the masks by type. On SE it is CommonLib's `0xB10`, which SE `GetNumTints`
  (ID 39614) also reads. Each mask maps to the race's `TintAsset` for the sex by
  texture (pointer, then case-insensitive path) for its stable `index`.
  Zero-alpha masks and masks without an asset are skipped.

A capture that differs from the last published one must repeat in two
consecutive captures, so dragging an editor slider does not stream updates.
Publishes are at least 3 s apart, each increments `revision`, and the last one
is republished on every new connection.

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
- race and original race, sex, weight, and height;
- the race's attack data map and the default combat style, which
  `TESNPC::InitItemImpl` fills only for NPCs loaded from plugins; weapon draws
  read the attack data map without a null check;
- the resolved head parts, hair color, face details, morphs, parts, body tint,
  and tint layers (`interpolationValue = alpha × 100`, with the matching
  preset).

Bases are never freed: a deleted reference may still point at its base for a
few frames, and the leak is bounded by the source's publish rate. A revision
that cannot be built keeps the previous base.

### Resolution and fallback

| Data | Unresolvable when | Fallback |
| --- | --- | --- |
| Race | plugin or form missing, not a race, or not playable | `defaultRace` if it resolves to a playable race, else `NordRace` (`Skyrim.esm` `013746`) |
| Head part | missing, wrong type, or its valid-race list rejects the race | the race's default part of that type and sex, else omitted |
| Face part | none after resolution | the race's default face |
| Hair color | color form creation fails | the race's default hair color |
| Face details | missing | the race's default face details |
| Tint layer | no race tint asset with that index and type | dropped |

A fallback race takes the sex of `defaultRace`, or the source's own without one,
and the race's default look for that sex: default head parts and skin tone. The
source's head parts, tints, morphs, presets, face details, and hair color belong
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
  or with `bUseFaceGenPreprocessedHeads` off, the same on both runtimes, which
  left the dark face bug. The receiver
  finds the face geometry (named after the face part's editor ID), takes its
  FaceGen material's tint texture, and queues the engine's tint render
  (ID 27040, SE 26454) with masks from the base's layers: race `TintAsset`
  texture, layer color, and `interpolationValue / 100` alpha, at most 16
  non-zero layers. It renders again whenever the head gets a new tint texture,
  such as after a 3D reset.

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
senders, and [nameplates](ui-overlay.md#nameplates): the display name, or a placeholder for an empty one, as before a
peer's appearance arrives.

The level is not part of the label. It stays in `Appearance` and `ChatSpeaker`
([Network](network.md#appearance)), captured, validated, relayed, and parsed
as before, for a later use.

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

Capture, skipped for a non-playable race:

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
- `ammo`: the worn `TESAmmo` entry, since `Actor::GetCurrentAmmo` returns nothing
  for the player.
- `defaultOutfit`: the pieces for items that are not from base-game plugins
  ([Default outfit](#default-outfit)).

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
too, such as the Anniversary backpack on an install without it.

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
`Forms::GetItemTraits` reads them from a form, and `Forms::ItemFilter::Matches`
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

To verify in game:

2. `CreateReferenceAtLocation` completes 3D and graph initialization for a base
   built from engine defaults, and the firewall, combat suppression, and
   furniture entry behave as with the earlier copied base.
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

To verify in game:

11. A factory-built `TESObjectLIGH` with copied components and data equips as a
    torch, raises the torch arm, and shows the same flame, light, and flicker,
    and a spell's hand light accepts it.

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
   source's default race and sex with the race's default look, and no crash.
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

## Known limitations

- Voice type, actor values, transformation races and their graphs, and
  RaceMenu/NiOverride data (body morphs, overlays, node transforms, scaled bones)
  are not synchronized. Custom skin armor and far skins use the resolved race's.
- Enchantments, tempering, poison, charge, player-given names, the equipped shout
  or power, and SMP/CBPC physics are not shown.
- An outfit piece stands in only for its slot and weapon type, not for the
  missing item's look. A default race shows its own default face.
- Tint capture is off on VR.
- Lights built into a mesh (an `NiLight` in the model, not a `TESObjectLIGH`),
  such as some modded armor or weapons, keep their shadows.
