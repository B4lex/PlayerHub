# Configuration

## Status

Implemented; the first file-based version ran in game. On Linux,
`PluginConfig.cpp` and `SettingsSchema.cpp` were compiled against SimpleIni
with stubbed SKSE headers and checked for default-file generation, overrides,
invalid values, unknown keys, and saving: comments and unknown keys are kept,
and a missing file is recreated from the commented default. Applying changes in
game (reconnect, equipment re-resolution, spawn rate) is not yet checked.

## Scope

`skyrim-mp.ini` is the plugin's general configuration. This system owns the
file, its schema, the immutable settings snapshot, how consumers follow a
change, and saving. The settings window that edits it is in
[UI overlay](ui-overlay.md#settings-window); what each value does is in its
consumer's system.

## Components

| Module | Responsibility |
| --- | --- |
| `config/PluginConfig` | `Config::PluginConfig`, `Load`, `Get`, `GetRevision`, `Apply`, saving |
| `config/SettingsSchema` | `Config::Describe`: every setting described once, through a visitor |

## File

`skyrim-mp.ini` sits in the directory of the loaded plugin DLL, found through
the DLL's own module path, so it also works from a mod manager's folder. It is
read once when SKSE loads the plugin, before any other plugin work.

- A missing file is written with every default and comments describing each key.
  An existing file is never rewritten on load, so a newer plugin never
  overwrites a user's edits.
- SimpleIni reads and writes it (`xmake.lua`), without a Unicode converter: the
  file is UTF-8 bytes.
- An absent key keeps its default; an invalid value is logged and keeps its
  default; keys no setting reads, including keys outside any section, are logged
  as unknown; a duplicate key takes the last value.
- Section and key names are case-insensitive. Comments start with `;` or `#` on
  their own line: SimpleIni keeps a trailing comment in the value, so such a
  value fails validation instead of being silently misread.
- External edits apply on restart; the settings window applies its changes at
  once.

### Schema

Each setting is described once in `SettingsSchema.cpp`, through a visitor that
reads the file, writes it, and draws the settings window. A new option is a field
in `Config::PluginConfig` plus one line there. `Config::SettingsVisitor::Form`
takes the setting's item filter
([Appearance and equipment](appearance-and-equipment.md#item-filter-and-catalog)).

## Settings

### `[Network]`

| Key | Meaning | Default |
| --- | --- | --- |
| `ServerEndpoint` | Server URL starting with `http://` or `https://`, which the client connects to as `ws://` or `wss://` at `/ws` when the URL has no path; replaces `skyrim-mp-server.txt`, which is no longer read | the public server |
| `AutoConnect` | Whether each game connects when it loads; otherwise the settings window's Connect does | `true` |
| `ViewRadius` | Game units within which other players are shown, `0..100000`; sent in `Hello` and clamped by the server ([Interest and delivery](interest-and-delivery.md#subscriptions)); applies on the next connection | `0`, the server's range |

### `[DefaultOutfit]`

What peers show in place of this player's race and items they cannot resolve
([Appearance and equipment](appearance-and-equipment.md#default-outfit)). Form
values are `Plugin|FormID`, the defining plugin and its local hexadecimal ID, as
in `Skyrim.esm|0x012E49`. A full load-order form ID is rejected, since it differs
between modpacks. Every key has a value: an empty or malformed one is invalid and
keeps the default. Only base-game forms, including Anniversary Edition content,
are accepted.

| Key | Meaning |
| --- | --- |
| `Race` | a playable race; base-game plugins define only the 10 vanilla ones |
| `Sex` | `Male` or `Female`, shown with `Race` |
| `<Slot>` | armor for that biped slot; `Head` also stands in for `Hair` |
| `OneHandSword` … `Crossbow` | the weapon for that Creation Kit weapon type |
| `Torch`, `Arrow`, `Bolt` | carried torch and ammunition |

Every biped slot is synchronized, but only the most visible have an outfit key:
`Head`, `Body`, `Hands`, `Amulet`, `Feet`, `Shield`, `Circlet`, and `Backpack`
for slot 47, which Anniversary Edition's `ccFSVSSE001-Backpacks.esl` and most
mods use. A modded item covering none of these, such as a ring, is left off.

Defaults, from `Skyrim.esm` unless noted: `Race` `NordRace`, `Sex` `Male`.

| Key | Default |
| --- | --- |
| `Head` | `ArmorHideHelmet` |
| `Body` | `ArmorHideCuirass` |
| `Hands` | `ArmorHideGauntlets` |
| `Feet` | `ArmorHideBoots` |
| `Shield` | `ArmorHideShield` |
| `Amulet` | `JewelryNecklaceGold` |
| `Circlet` | `ClothesCirclet01` |
| `Backpack` | `ccFSVSSE001_Backpack_Basic` (`ccFSVSSE001-Backpacks.esl`; vanilla has no backpacks) |
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
| `SpawnsPerSecond` | Remote actor and mount spawns started per second at most, `0..1000`; `0` is unlimited ([Sync pipeline](sync-pipeline.md#actor-lifecycle)) | `20` |

### `[UI]`

| Key | Meaning | Default |
| --- | --- | --- |
| `SettingsKey` | Opens the settings window: `F1`–`F12`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, or `ScrollLock` | `F10` |
| `StatusPanel` | Shows the status panel | `true` |
| `ItemPreview` | Shows the 3D item preview in item pickers | `true` |

## Snapshots

`Config::Get()` returns a `shared_ptr` to an immutable snapshot held in an
`std::atomic<std::shared_ptr>`, safe from any thread. `Config::Apply` publishes a
new snapshot, advances `Config::GetRevision()`, then saves. A consumer compares
the revision with the one it last saw and only then reads the snapshot, so an
unchanged configuration costs one atomic load per pass, and one that reads the
revision before the snapshot never misses a change.

| Consumer | Thread | Follows a change by |
| --- | --- | --- |
| `ActorLifecycle` | main loop | reading `SpawnsPerSecond` once per pass in `RefillSpawnAllowance` |
| `DefaultOutfit` | main loop | `Refresh`, first in `SyncCoordinator::OnFrame`, resolving every item again when `[DefaultOutfit]` differs |
| `EquipmentSync` | main loop | the next capture selecting from the new outfit and publishing a new revision |
| `AppearanceSync` | main loop | the next capture taking the new default race and sex, which for a modded race publishes a new revision |
| `ConnectionMonitor` | main loop | reconnecting at once when the latest attempt went to another endpoint; reading `AutoConnect` when a game starts |
| `ServerClient` | transport, worker | reading the endpoint for each attempt |
| `SettingsWindow` | overlay, input | reading `SettingsKey` on each focus key press |
| `StatusPanel`, item pickers | overlay | reading `StatusPanel` and `ItemPreview` each frame |

## Saving

- The current file is loaded with its comments and each setting written into it.
  SimpleIni keeps an existing key's comment and position, adds a new key with its
  description at the end of its section, and a missing section with its comment.
  Entries no setting reads are kept.
- A setting absent from the file and equal to its default stays absent, so a
  removed line keeps meaning "default".
- A missing or unreadable file is recreated from the commented default.
- The text goes to `skyrim-mp.ini.tmp` and is renamed over the file, so a failed
  write never truncates it. A failed save is logged and reported in the window;
  the applied settings still last until restart.

## Acceptance criteria

1. Without a file, the first launch writes `skyrim-mp.ini` next to the DLL with
   the defaults; later launches never overwrite it.
2. Invalid values, unknown keys, and configured forms that do not resolve or are
   not base-game items are logged and fall back to their defaults, or to no piece
   when the default is unusable too.
3. `[Network] ServerEndpoint` sets the server URL.
   `[Performance] SpawnsPerSecond` bounds spawns, so many players arriving at once do not spawn
   in one frame.
4. Applying changes takes effect without a restart, and the file keeps
   hand-written comments.
