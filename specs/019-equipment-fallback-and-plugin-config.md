# Equipment Fallback and Plugin Configuration

## Status

Implemented and run in game. On Linux, `PluginConfig.cpp` was compiled against
SimpleIni with stubbed SKSE logging and checked for default-file generation,
overrides, invalid values, and unknown keys. The default fallback form IDs were checked
against `Skyrim.esm` and `Dawnguard.esm` records: type, slots, armor type, and
weapon type.

## Goal

A remote player whose equipment comes from a mod the receiver does not have
still appears dressed and armed. Each unresolvable item is replaced by a local
item for the same slot: a heavy cuirass by a heavy cuirass, a greatsword by a
greatsword. Before this change, spec 007 dropped such items, which left actors
in the race's naked skin or unarmed.

The replacements are configured in an INI file next to the plugin DLL. That file
is the plugin's general configuration and also holds the server endpoint, which
replaces `skyrim-mp-server.txt`.

## Non-goals

- Matching the look of the missing item (material, color, style). A fallback
  only stands in for its slot and weapon type.
- Protocol changes. The fallback uses the hints spec 007 already sends:
  `slotMask` and `armorType` for armor, `kind` and `weaponType` for hands.
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

### `[EquipmentFallback]`

Form values are `Plugin|FormID`: the defining plugin and the hexadecimal form ID
local to it, as in `Skyrim.esm|0x012E49`. A full load-order form ID is rejected,
because it differs between modpacks. An empty value means no fallback.

| Key | Meaning |
| --- | --- |
| `Enabled` | `false` restores spec 007's behavior of leaving missing items off |
| `<Slot>.Light`, `<Slot>.Heavy`, `<Slot>.Clothing` | armor for that biped slot and armor type |
| `<Slot>` | armor for that slot when the type's own key is empty |
| `OneHandSword` … `Crossbow` | weapon for that Creation Kit weapon type |
| `Torch`, `Arrow`, `Bolt` | carried torch and ammunition |

Slots are the synced slots of spec 007, by Creation Kit name: `Head`, `Hair`,
`Body`, `Hands`, `Amulet`, `Feet`, `Shield`, and `Circlet`. Jewelry looks the
same whatever its armor type, so `Amulet` and `Circlet` have only the `<Slot>`
key.

Defaults, all from `Skyrim.esm` unless noted, by editor ID:

| Slot | Light | Heavy | Clothing | Any type |
| --- | --- | --- | --- | --- |
| Head, Hair | `ArmorHideHelmet` | `ArmorIronHelmet` | `ClothesCollegeHood` | |
| Body | `ArmorHideCuirass` | `ArmorIronCuirass` | `ClothesFarmClothes01` | |
| Hands | `ArmorHideGauntlets` | `ArmorIronGauntlets` | | |
| Feet | `ArmorHideBoots` | `ArmorIronBoots` | `ClothesFarmBoots01` | |
| Shield | `ArmorHideShield` | `ArmorIronShield` | | |
| Amulet | | | | `JewelryNecklaceGold` |
| Circlet | | | | `ClothesCirclet01` |

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

## Receiver

### Resolving the configuration

At `kDataLoaded`, and again whenever the settings window changes
`[EquipmentFallback]`, every configured form is resolved and checked with the
key's item filter (spec 022): armor of the key's armor type covering the key's
slot, where Head, Hair, and Circlet count as one group; a
`TESObjectWEAP` of exactly the key's weapon type; a carriable `TESObjectLIGH` for
the torch; and a `TESAmmo` that is or is not a bolt. A form that is missing or
does not fit is logged with its key and treated as unset. After that, a lookup is
an array index.

### Armor

Spec 007's resolution filter is unchanged. Its drop branch now selects fallbacks:

1. All resolvable armor is resolved first, and the union of its slot masks
   becomes the occupied slots.
2. For each unresolved entry, the slots of its `slotMask` are visited in
   priority order: Body, Head, Hair, Hands, Feet, Shield, Amulet, Circlet. For a slot that is not occupied yet, the fallback is the key for the
   entry's armor type, or else the slot's any-type key.
3. A fallback whose own slot mask overlaps an occupied slot is skipped, because
   equipping it would unequip an item already shown. Otherwise it is added, and
   its slots become occupied.

So a missing cuirass with built-in gauntlets and boots gets a body, hands, and
feet fallback, except where the player shows real gauntlets or boots. When the
fallbacks of one missing item overlap, the earlier slot wins, so the most
visible pieces go first. A missing full helmet (Head and Hair) gets one vanilla
helmet: the Head fallback covers Hair, so Hair is already occupied.

### Hands and ammunition

An unresolved weapon uses the fallback for its `weaponType`, so the actor keeps
the source's attack animations. An unresolved torch uses `Torch`. A two-handed
fallback still clears the left hand, as in spec 007.

Ammunition carries no type hint. When it is unresolved, the fallback is `Bolt`
if the source's right hand is a crossbow, and `Arrow` otherwise.

### Logging

Each unresolved entry is logged once per revision, with the resolved fallback
form IDs, or `none`.

## Acceptance criteria

1. With no INI file, the first launch writes `skyrim-mp.ini` next to the DLL
   with the defaults. Later launches never overwrite it.
2. A remote player wearing armor from a plugin the receiver lacks shows vanilla
   armor of the same armor type in the covered slots, without displacing items
   that did resolve.
3. A missing weapon shows the fallback of the same weapon type in the same hand,
   and attacks use that type's animations.
4. A missing torch or ammunition shows the configured torch, arrows, or bolts.
5. `Enabled = false`, or an empty value, leaves the item off as in spec 007.
6. Invalid values, unknown keys, and configured forms that do not resolve are
   logged and fall back to their defaults or to no fallback.
7. `[Network] ServerEndpoint` sets the server URL. `skyrim-mp-server.txt` is no
   longer read.
8. `[Performance] SpawnsPerSecond` bounds how many remote actors and mounts are
   spawned per second, so many players arriving at once do not spawn in one
   frame.
