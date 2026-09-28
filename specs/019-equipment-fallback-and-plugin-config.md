# Default Outfit and Plugin Configuration

## Status

Implemented and run in game as receiver-configured equipment fallbacks. Reworked
into the sender's default outfit, then extended with a default race and sex;
neither is built on Windows or run in game yet. On Linux, `PluginConfig.cpp` was compiled against SimpleIni with stubbed
SKSE logging and checked for default-file generation, overrides, invalid values,
and unknown keys. The default form IDs were checked against `Skyrim.esm` and
`Dawnguard.esm` records: type, slots, and weapon type.

## Goal

A remote player whose equipment comes from a mod the receiver does not have
still appears dressed and armed. Each unresolvable item is replaced by an item
for the same slot: a cuirass by a cuirass, a greatsword by a greatsword. Before
this change, spec 007 dropped such items, which left actors in the race's naked
skin or unarmed. Likewise, a player of a modded race the receiver does not have
appears as a vanilla race and sex of the player's choice, rather than as a
`NordRace` with the player's own sex (spec 006).

The replacements are the source player's choice, not the receiver's: each player
configures a default outfit, which peers show in place of that player's race
and items they cannot resolve. Default outfit forms are limited to base-game
plugins: the
Special Edition masters (`Skyrim.esm`, `Update.esm`, `Dawnguard.esm`,
`HearthFires.esm`, and `Dragonborn.esm`) and the 74 Creation Club plugins
Anniversary Edition bundles, as its `Skyrim.ccc` lists them. Every receiver has
the masters. A Special Edition receiver without the Anniversary upgrade lacks
the Creation Club plugins and shows nothing for pieces from them.

The outfit is configured in an INI file next to the plugin DLL. That file is the
plugin's general configuration and also holds the server endpoint, which
replaces `skyrim-mp-server.txt`.

## Non-goals

- Matching the look of the missing item (material, color, style, or armor
  type). An outfit piece only stands in for its slot and weapon type.
- Keeping the face across races. A default race is shown with its own default
  look (spec 006).
- A receiver-side setting. When the source's outfit has no piece for an item,
  or the piece does not resolve, the receiver shows nothing there.
- Reloading the file while the game runs. Edits to the file apply on restart;
  the in-game settings window of spec 021 applies its changes at once.

## Configuration file

`skyrim-mp.ini` in the directory of the loaded plugin DLL, found through the
DLL's own module path, so it also works from a mod manager's mod folder. It is
read once when SKSE loads the plugin, before any other plugin work.

- If the file does not exist, the plugin writes one with every default value and
  comments that describe the keys. Nothing is written when the file exists, so a
  newer plugin never overwrites a user's edits.
- The file is read and written with SimpleIni (`xmake.lua`, no Unicode
  converter: the file is UTF-8 bytes).
- An absent key keeps its default. An invalid value is logged and keeps its
  default. Keys no setting reads, including keys outside any section, are
  logged as unknown. A duplicate key takes the last value.
- Section and key names are case-insensitive. Comments start with `;` or `#` and
  must be on their own line: SimpleIni keeps a trailing comment in the value, so
  such a value fails validation instead of being silently misread.

### Extensibility

Each setting is described once, in `SettingsSchema.cpp`, through a visitor that
reads the file, writes it, and draws the in-game settings window (spec 021). A
new option is a field in `Config::PluginConfig` plus one line in that
description. Features read their typed settings through `Config::Get()`, an
immutable snapshot that is safe from any thread; spec 021 describes how a
feature follows a changed snapshot.

### `[Network]`

| Key | Meaning | Default |
| --- | --- | --- |
| `ServerEndpoint` | Server URL, starting with `http://` or `https://` | the public server |
| `AutoConnect` | Whether each game connects when it loads; otherwise the settings window's Connect button does | `true` |

### `[DefaultOutfit]`

Form values are `Plugin|FormID`: the defining plugin and the hexadecimal form ID
local to it, as in `Skyrim.esm|0x012E49`. A full load-order form ID is rejected,
because it differs between modpacks. Every key has a value: an empty or
malformed value is invalid and keeps the default. Only base-game forms,
including Anniversary Edition content, are accepted.

| Key | Meaning |
| --- | --- |
| `Race` | a playable race; the base-game plugins define only the 10 vanilla ones |
| `Sex` | `Male` or `Female`, shown with `Race` |
| `<Slot>` | armor for that biped slot; `Head` also stands in for `Hair` |
| `OneHandSword` … `Crossbow` | weapon for that Creation Kit weapon type |
| `Torch`, `Arrow`, `Bolt` | carried torch and ammunition |

Every biped slot is synced (spec 007), but only the most visible ones have an
outfit key. By Creation Kit name, with `Hair` folded into `Head`: `Head`,
`Body`, `Hands`, `Amulet`, `Feet`, `Shield`, and `Circlet`, plus `Backpack` for
slot 47, which Anniversary Edition's `ccFSVSSE001-Backpacks.esl` and most mods
use for backpacks. A modded item that covers none of these slots, such as a
ring or another mod slot, is left off.

Defaults, all from `Skyrim.esm` unless noted, by editor ID: `Race` is
`NordRace` and `Sex` is `Male`, as spec 006's last resort was.

| Slot | Default |
| --- | --- |
| Head | `ArmorHideHelmet` |
| Body | `ArmorHideCuirass` |
| Hands | `ArmorHideGauntlets` |
| Feet | `ArmorHideBoots` |
| Shield | `ArmorHideShield` |
| Amulet | `JewelryNecklaceGold` |
| Circlet | `ClothesCirclet01` |
| Backpack | `ccFSVSSE001_Backpack_Basic` (`ccFSVSSE001-Backpacks.esl`, Anniversary Edition; vanilla has no backpacks) |

| Key | Default |
| --- | --- |
| `OneHandSword`, `OneHandDagger`, `OneHandAxe`, `OneHandMace` | `IronSword`, `IronDagger`, `IronWarAxe`, `IronMace` |
| `TwoHandSword`, `TwoHandAxe` | `IronGreatsword`, `IronBattleaxe` |
| `Bow`, `Staff` | `HuntingBow`, `StaffTemplateDestruction` (unenchanted) |
| `Crossbow` | `DLC1CrossBow` (`Dawnguard.esm`) |
| `Torch`, `Arrow` | `Torch01`, `IronArrow` |
| `Bolt` | `DLC1BoltSteel` (`Dawnguard.esm`) |

Vanilla helmets and hoods cover `Hair` and `Circlet`, not `Head`.

### `[Performance]`

| Key | Meaning | Default |
| --- | --- | --- |
| `SpawnsPerSecond` | Remote actor and mount spawns started per second at most, `0` to `1000`; `0` is unlimited | `20` |

Every spawn in a main-loop pass draws from one allowance, refilled by the rate
times the frame time. A pass may always start one spawn, but never saves up more
than its own share. A due spawn waits until it has waited as long as the
longest spawn held back in the previous pass, so the oldest waits go first
regardless of peer order.

## Source

### Resolving the configuration

At `kDataLoaded`, and again whenever the settings window changes
`[DefaultOutfit]`, every configured form is resolved and checked with the key's
item filter (spec 022): a base-game form that is a playable race, armor
covering the key's slot,
where Head, Hair, and Circlet count as one group; a `TESObjectWEAP` of exactly
the key's weapon type; a carriable `TESObjectLIGH` for the torch; and a
`TESAmmo` that is or is not a bolt. A form that is missing or does not fit is
logged with its key and replaced by the key's default, or treated as unset
when the default is unusable too, such as the Anniversary Edition backpack on
a Special Edition install without the upgrade.

### Selecting the pieces

Items from base-game plugins are expected to resolve on receivers, so only the
other items need a stand-in. Each equipment capture (spec 007) selects the outfit
pieces for them and publishes them with the equipment as `defaultOutfit`:

- `armor`: the piece of every outfit slot that a modded worn item covers, in
  priority order: Body, Head (for Head or Hair), Hands, Feet, Shield, Amulet,
  Circlet, Backpack. Each piece carries the slot mask of the outfit slot it stands in
  for, not its own slots.
- `hands`: the weapon for the weapon type of each modded hand weapon, and the
  torch for a modded torch, each at most once.
- `ammo`: the `Bolt` or `Arrow` piece for modded bolts or arrows.

A player whose equipment is all base-game sends an empty outfit. A changed
outfit or setting changes the next capture, which publishes a new revision.

Each appearance capture (spec 006) likewise sets `defaultRace` to the resolved
`Race` and the `Sex` when the player's race is from other than a base-game
plugin, and to `null` otherwise. A changed race or sex changes the next
capture, which publishes a new appearance revision, so every receiver respawns
the player once.

## Receiver

The received outfit is part of the equipment revision, so a new outfit resolves
the equipment again as any new revision does. The receiver has no outfit of its
own: when the source's outfit has no piece for an unresolved item, or the piece
does not resolve to an item of the expected type, nothing is shown there.

### Armor

Spec 007's resolution filter is unchanged. Its drop branch now selects outfit
pieces:

1. All resolvable armor is resolved first, and the union of its slot masks
   becomes the occupied slots.
2. For each unresolved entry, the outfit pieces are visited in their order. A
   piece stands in when the entry covers a slot of the piece's slot mask that
   is not fully occupied yet.
3. A piece whose own slot mask overlaps an occupied slot is skipped, because
   equipping it would unequip an item already shown. Otherwise it is added, and
   its slots become occupied.

So a missing cuirass with built-in gauntlets and boots gets a body, hands, and
feet piece, except where the player shows real gauntlets or boots. When the
pieces for one missing item overlap, the earlier slot wins, so the most visible
pieces go first. A missing helmet or hood, whether it covers Head, Hair, or
both, gets the one Head piece.

### Hands and ammunition

An unresolved weapon uses the outfit weapon of its `weaponType`, so the actor
keeps the source's attack animations. An unresolved torch uses the outfit torch.
A two-handed stand-in still clears the left hand, as in spec 007. Unresolved
ammunition uses the outfit ammunition, which the source picked by its own ammo
type.

### Logging

Each unresolved entry is logged once per revision, with the form IDs of the
resolved stand-ins as `standIn`, or `none`.

## Protocol

`player:appearance` carries an optional `defaultRace`,
`{"race": {"plugin": "Skyrim.esm", "formId": 79685}, "female": true}`, whose
race must be from a base-game plugin (spec 006).

`player:equipment` carries an optional `defaultOutfit` object:

```json
{
  "armor": [{ "plugin": "Skyrim.esm", "formId": 80145, "slotMask": 4 }],
  "hands": [{ "plugin": "Skyrim.esm", "formId": 77495, "kind": "weapon", "weaponType": 1 }],
  "ammo": null
}
```

At most 8 armor pieces with a nonzero slot mask, and at most 2 hands, each a
`weapon` with a weapon type from 1 to 9 or a `torch`, with no two for the same
kind and weapon type. Every reference must be from a base-game plugin, compared
without case: `Multiplayer::IsBaseGamePlugin` and the server's
`BASE_GAME_PLUGINS` hold the same list. The server and the client reject equipment whose outfit breaks
these rules. An absent outfit, as in recordings made before this change, is
empty; the server always relays one.

## Acceptance criteria

1. With no INI file, the first launch writes `skyrim-mp.ini` next to the DLL
   with the defaults. Later launches never overwrite it.
2. A remote player wearing armor from a plugin the receiver lacks shows that
   player's default outfit armor in the covered slots, without displacing
   items that did resolve.
3. A missing weapon shows the source's outfit weapon of the same weapon type in
   the same hand, and attacks use that type's animations.
4. A missing torch or ammunition shows the source's outfit torch, arrows, or
   bolts.
5. A source outfit piece that the source cannot use, even as its default,
   leaves the item off on receivers, as in spec 007.
6. A remote player of a race from a plugin the receiver lacks appears as that
   player's default race and sex, with the race's default look.
7. The settings window lists only base-game items and races, including
   Anniversary
   Edition content such as backpacks, for outfit keys. Invalid
   values, unknown keys, and configured forms that do not resolve or are not
   base-game items are logged and fall back to their defaults, or to no piece
   when the default is unusable too.
8. Changing the outfit in the settings window re-dresses the local player on
   peers within about a second.
9. `[Network] ServerEndpoint` sets the server URL. `skyrim-mp-server.txt` is no
   longer read.
10. `[Performance] SpawnsPerSecond` bounds how many remote actors and mounts
    are spawned per second, so many players arriving at once do not spawn in
    one frame.
