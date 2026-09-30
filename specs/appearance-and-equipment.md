# Appearance and Equipment

## Status

- **Appearance:** run in game between two clients on Skyrim AE 1.7.104 with a
  base copied from the local player base. The current base, built from engine
  defaults, and the default race are not yet built on Windows or run in game.
- **Equipment:** run in game, including an earlier receiver-configured
  fallback. The items under [Engine assumptions](#engine-assumptions) are not
  individually checked.
- **Default outfit:** the sender's outfit is not yet built on Windows or run in
  game. The default form IDs were checked against `Skyrim.esm` and
  `Dawnguard.esm` records (type, slots, weapon type).
- **Item filter and catalog:** built on Windows; the catalog and picker run in
  game ([UI overlay](ui-overlay.md#item-picker)).
- **Player labels:** name index, level validation, and chat fields covered by
  server unit tests; label rendering pending an in-game check.

## Scope

A remote actor looks like its source player's character, carries its name and
level, and wears and wields what the source has equipped, from any plugin the
receiver also has. What the receiver cannot resolve falls back to vanilla data
or to the source's own default outfit, so players with different modpacks still
see each other dressed and armed.

The wire format is in [Network](network.md#appearance-and-equipment). How the
draw and attacks wait for the hands is in
[Combat presentation](combat-presentation.md). Where the outfit is configured is
in [Configuration](configuration.md#defaultoutfit).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/appearance/AppearanceSync` | Capture and publish; receive, build the actor base, respawn requests, hair, skin, and face tint presentation |
| `sync/equipment/EquipmentSync` | Capture and publish; receive, resolve, reconcile the actor's inventory; hand readiness |
| `sync/outfit/DefaultOutfit` | Resolve `[DefaultOutfit]` and select the pieces for a capture |
| `sync/PlayerLabel.h` | `PlayerLabel::Format` |
| `protocol/AppearanceState.h`, `EquipmentState.h`, `FormReference.h` | Plain data and validators shared by capture and the network parser |
| `forms/FormLookup` | Encode and resolve `{plugin, formId}` |
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
- `name`: the base full name with control characters removed, cut at a UTF-8
  code point boundary to 128 bytes. `level`: `Actor::GetLevel`, so a level-up
  publishes a new revision.
- `defaultRace`: the configured `[DefaultOutfit]` race and sex when `race` is
  not from a base-game plugin, else unset.
- `female`, `weight`, `height`, `headParts[0..numHeadParts)`, the hair color
  form's RGB, `faceDetails`, and `faceData` morphs and parts.
- `bodyTint`: `TESNPC::bodyTintColor`, or the skin tone mask's color if black.
- `tints`: `PlayerCharacter` runtime `tintMasks` in blend order. On 1.7.104 the
  array is at `0xB20` (CommonLibSSE-NG reads `0xB18`); it is used only when it
  matches `GetNumTints`/`GetTintMask` (IDs 40700/40698), which otherwise list the
  masks by type. Each mask maps to the race's `TintAsset` for the sex by texture
  (pointer, then case-insensitive path) for its stable `index`. Zero-alpha masks
  and masks without an asset are skipped.

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
to its own race and sex and are dropped; name, level, weight, and height are
kept. Each fallback is logged once per revision.

### Spawn and respawn

- An actor waits up to 3 s after its first checkpoint for an appearance, then
  spawns with a default base: `NordRace` male with the race's default look and
  the label `Unknown`.
- A newer revision retires the actor at the next pass, at least 2 s after the
  previous respawn for that player. The respawn uses the new base and rebuilds
  locomotion, stance, and furniture from the played checkpoint. Coordinate-space
  respawns reuse the current base.
- A change of only name, level, or name index rewrites the built base's full
  name in place and adopts the revision without a new base or respawn.
- Once the respawned actor is ready with 3D, `Actor::UpdateHairColor` and
  `UpdateSkinColor` run once.
- The face tint (AE only): `PrepareHeadPartForShaders` (ID 26838) gives a
  runtime-built face a blank tint texture and fills it only for the player base
  or with `bUseFaceGenPreprocessedHeads` off, which left the dark face bug. The
  receiver finds the face geometry (named after the face part's editor ID),
  takes its FaceGen material's tint texture, and queues the engine's tint render
  (ID 27040) with masks from the base's layers: race `TintAsset` texture, layer
  color, and `interpolationValue / 100` alpha, at most 16 non-zero layers. It
  renders again whenever the head gets a new tint texture, such as after a 3D
  reset.

### Player label

`PlayerLabel::Format` builds `Name #N • L lvl`, used for the actor base's name
and for chat senders:

```text
Lydia • 12 lvl
Lydia #2 • 101 lvl
```

An empty name shows as `Unknown`. `#N` appears only for an index above 1, and
the level only when known (non-zero). The bullet is U+2022, written as UTF-8
bytes. The index comes from the server ([Network](network.md#name-index)).

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
  left hand is `torch`; a `SpellItem` or `ScrollItem` is a `SPELL` hand,
  whose spell is never sent; bare fists or anything else is an unset hand. A two-handed
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
resolves, as the receiver's version. A spell hand resolves to the placeholder
spell ([Combat presentation](combat-presentation.md#spells)). Each drop is
logged once per revision with player, plugin, form ID, and reason (`missing`,
`wrong-type`). The filter is one policy function returning a form or a drop
reason, whose drop branch selects the default outfit.

### Reconciliation

The actor's inventory must always equal the resolved set. The runtime base has
no container or outfits, and reconciliation is the authority for everything
else. It runs for a ready, 3D-loaded actor and compares live inventory counts,
worn armor, and equipped hand objects with the resolved set. When they differ:

1. Required counts are one per armor entry and one per hand object (two when
   dual-wielding one weapon), plus 20 of the resolved ammo.
2. Every other object, and any excess, is removed and deleted, never
   transferred to a container or the player: gold, leveled items, script items.
3. Equipped armor and hand objects outside the set are unequipped. A placeholder
   spell in a hand that should be empty is removed with
   `ActorEquipManager::UnequipSpell` (ID 38902, Papyrus `Actor.UnequipSpell`),
   whose `ActionForceEquip` lowers a drawn hand; `Actor::DeselectSpell` sent no
   graph event and left the hand raised, and remains the fallback off AE.
4. Missing objects are added.
5. Armor, then the right hand, then the left, are equipped with
   `ActorEquipManager::EquipObject` in the matching `BGSEquipSlot` (either for
   two-handed), `queueEquip=false`, `forceEquip=true`, `playSounds=false`. A
   spell hand uses `EquipSpell` in its slot and never counts as inventory. The
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
base-game plugins, which every receiver has (a Special Edition receiver without
the Anniversary upgrade lacks the Creation Club ones and shows nothing for
them), so they are expected to resolve. It is the source's choice; the receiver
has no outfit of its own.

`DefaultOutfit` resolves every configured form at `kDataLoaded` and whenever
the settings change `[DefaultOutfit]` (`Refresh`, first in each pass), checked
with the key's item filter. A form that is missing or does not fit is logged
with its key (`Default outfit item ignored ... reason=does-not-fit`) and
replaced by the key's default, or treated as unset when the default is unusable
too, such as the Anniversary backpack on a Special Edition install.

Each equipment capture selects pieces only for items not from base-game
plugins:

- `armor`: the piece of each outfit slot a modded worn item covers, in priority
  Body, Head (for Head or Hair), Hands, Feet, Shield, Amulet, Circlet, Backpack.
  Each piece carries the slot mask of the outfit slot it stands in for.
- `hands`: the weapon of each modded hand weapon's type and the torch for a
  modded torch, each at most once.
- `ammo`: the `Bolt` or `Arrow` piece for modded bolts or arrows.

An all-base-game player sends an empty outfit. A changed outfit or setting
changes the next capture and publishes a new revision, as does a changed default
race or sex in the next appearance capture (every receiver then respawns the
player once).

The receiver treats the outfit as part of the equipment revision:

1. All resolvable armor is resolved first, and the union of its slot masks
   becomes the occupied slots.
2. For each unresolved armor entry, the pieces are visited in order. A piece
   stands in when the entry covers a slot of the piece's mask that is not yet
   fully occupied, and it is skipped when its own mask overlaps an occupied slot,
   since equipping it would unequip an item already shown. Added pieces occupy
   their slots. So a missing cuirass with built-in gauntlets gets body and hands
   pieces except where real gauntlets show, and a missing helmet or hood, on Head,
   Hair, or both, gets the one Head piece.
3. An unresolved weapon uses the outfit weapon of its `weaponType`, so attacks
   keep their animations; a two-handed stand-in still clears the left hand. An
   unresolved torch uses the outfit torch, and unresolved ammo the outfit ammo.
4. Without a piece, or when the piece does not resolve to the expected type,
   nothing is shown there. Each unresolved entry is logged once per revision with
   its stand-ins as `standIn`, or `none`.

## Item filter and catalog

`Forms::ItemTraits` decides whether an item can stand in for another: its kind
(armor, weapon, light, ammo, or a playable race as `kRace`), slot mask, weapon
type, bolt flag, and whether a base-game plugin defines it.
`Forms::GetItemTraits` reads them from a form, and `Forms::ItemFilter::Matches`
checks them against what a setting accepts. `GetOutfitArmorFilter`,
`GetOutfitWeaponFilter`, and `Config::kOutfitTorchFilter`, `kOutfitArrowFilter`,
`kOutfitBoltFilter`, `kOutfitRaceFilter` build every `[DefaultOutfit]` filter.
The settings window lists only matching items and `DefaultOutfit` resolves with
the same filters, so the list and the live behavior never disagree.

| Key | Accepts (base-game only) |
| --- | --- |
| `<Slot>` | armor covering the slot |
| `Head`, `Circlet` | armor covering Head, Hair, or Circlet, because vanilla helmets cover Hair and Circlet yet are the default Head pieces |
| weapon keys | a weapon of exactly that type |
| `Torch` | a carriable light |
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

## Acceptance criteria

1. A remote actor shows the source's race, sex, weight, hair, eyes, hair color,
   face shape, skin and face tints, and its label, and wears the source's armor,
   clothing, jewelry, shield, hand items, and ammo from any plugin the receiver
   has. Late joiners see both without the source changing anything.
2. An appearance change (barber, RaceMenu) updates the actor within a few seconds
   with a single respawn, returning to its locomotion and furniture state; a name
   or level change relabels it without a respawn. An equipment change applies
   within 0.5 s without respawn, animation, or sound.
3. The actor's inventory holds exactly the synchronized items, also after a cell
   reload, a respawn, and a hit reaction. The local player's appearance,
   inventory, and equipment never change because of a remote actor.
4. A peer missing a race or head part plugin sees a vanilla fallback, or the
   source's default race and sex with the race's default look, and no crash.
5. Armor from a missing plugin shows the source's outfit armor in the covered
   slots without displacing items that resolved; a missing weapon, torch, or
   ammunition shows the outfit's of the same type, and attacks use that type's
   animations. Without a usable piece the item is left off.
6. Player-enchanted items appear as their unenchanted base item.
7. Drawing right after spawn or a swap draws the synchronized weapon, and the
   next attack uses its animation.
8. Two players with the same name show `Name` and `Name #2` over their actors
   and in chat.

## Known limitations

- Voice type, actor values, transformation races and their graphs, and
  RaceMenu/NiOverride data (body morphs, overlays, node transforms, scaled bones)
  are not synchronized. Custom skin armor and far skins use the resolved race's.
- Enchantments, tempering, poison, charge, player-given names, the equipped shout
  or power, and SMP/CBPC physics are not shown.
- An outfit piece stands in only for its slot and weapon type, not for the
  missing item's look. A default race shows its own default face.
- Appearance is synchronized only between 1.7.104 clients; tint capture is off on
  VR.
