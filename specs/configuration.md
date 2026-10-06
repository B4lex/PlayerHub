# Configuration

## Status

Implemented; the first file-based version ran in game. On Linux,
`PluginConfig.cpp` and `SettingsSchema.cpp` were compiled against SimpleIni
with stubbed SKSE headers and checked for default-file generation, overrides,
invalid values, unknown keys, and saving: comments and unknown keys are kept,
and a missing file is recreated from the commented default. Applying changes in
game (reconnect, equipment re-resolution, spawn rate) is not yet checked.
`[General] DisplayName` is not yet built on Windows or run in game. On Linux the
same two files were compiled against SimpleIni with stubs and checked for the
default file's empty `DisplayName` under its comment, loading it without a
warning, saving a display name, and ignoring a `Nickname` line, which an earlier
file may hold, as an unknown key. Clearing a display name and adding one to a
file without the key were checked before the key took this name. The current
character rule was checked by compile-time assertions only; `SettingsSchema.cpp`
with it was not compiled. `Config::GetDirectory` ran
only through its fallback, since the stubs resolve no DLL path.
`[UI] BorderColor` is not yet built on Windows or run in game. On Linux the same
two files were checked the same way: the default file has `BorderColor = Vanilla`
under its comment, `Accent` and `accent` load as the accent, `Orange` and an
empty value warn and keep `Vanilla`, a file without the key loads as `Vanilla`,
and applying `Accent` adds the line, which loads back. `[UI] BorderThickness`
and the limits of a percentage were checked the same way: the default file has
`BorderThickness = 70`, `50`, `85`, and `100` load, `49`, `101`, `150`, `0`,
`big`, and an empty value warn and keep `70` while `SpawnsPerSecond = 0` and
`ViewRadius = 100000` still load, and applying `85` adds the line, which loads
back. `ViewRadius` was checked as a `[Network]` key; reading and saving it in
`[Performance]` is not yet compiled or run, and a `[Network] ViewRadius` line an
earlier file holds is ignored as an unknown key.
`[UI] ChatWidth` and `ChatLines` are not yet built on Windows or run in game.
On Linux the same two files were checked the same way: the default file has
`ChatWidth = 30` and `ChatLines = 10` under their comments; `24`, `100`, `9`,
and `40` load; `23`, `101`, `8`, `41`, `0`, text, and an empty value warn and
keep the default; applying each adds its line to a file without it, keeping
the file's comments, and both load back.
`[UI] AccentColor` is not yet built on Windows or run in game. On Linux the
same two files ran with the settings window's harness
([UI overlay](ui-overlay.md#status)): the default file has
`AccentColor = FF9000` under its comment, before `BorderColor`; `#4f8ef7`
loads; `orange` warns and keeps `FF9000`; `ParseColor` takes `FF9000`,
`#4f8ef7`, and `000000` and rejects an empty value, `#`, five or seven
digits, `GG0000`, a sign, a space, `0x9000`, `##FF9000`, and a trailing
comment; applying a color from the window saves it, and applying `FF9000`
again keeps the line with that value.
`[UI] CloseChatOnSend` is not yet built on Windows or run in game. On Linux
the same two files ran with the settings window's harness: the default file
has `CloseChatOnSend = true` under its comment, between `StatusPanel` and
`ChatWidth`; `false` loads; `maybe` warns and keeps `true`; a file without
the key loads as `true`; switching it off and on in the window saves each
value.

## Scope

`playerhub.ini` is the plugin's general configuration. This system owns the
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

`playerhub.ini` sits in the directory of the loaded plugin DLL, found through
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
`Unsigned` takes the setting's maximum, above which a value in the file is
invalid, and a name for zero where zero is not a count, which only the settings
window shows; the file stores `0`. `Percent` takes both limits, outside which a
value in the file is invalid; the file stores the number without a sign.
`Color` is an opaque `0xRRGGBB` value that the file stores as six hexadecimal
digits (`FormatColor`, upper case); reading also takes lower case and a leading
`#` (`ParseColor`). It takes swatches, the colors the settings window offers.
`Layout` is a measure of a window that the player sets by dragging that window:
an unsigned value within both limits, read and saved as any other, which the
settings window does not show.

## Settings

### `[General]`

| Key | Meaning | Default |
| --- | --- | --- |
| `DisplayName` | The player's name as peers see it, in the player label over the actor and in chat ([Appearance and equipment](appearance-and-equipment.md#display-name)): 3..15 ASCII letters, digits, and underscores, as the server accepts (`Multiplayer::IsValidDisplayName`, [Network](network.md#sessions)). A file value that breaks the rule loads as not set. Empty means not set, and no connection is opened without one ([Network](network.md#connection)). It is the display name of the player's session, so the server refuses one another player holds ([Network](network.md#sessions)), and it reaches the server only when connecting: the settings window locks it while a connection is wanted or open ([UI overlay](ui-overlay.md#connection-controls)) | empty |

### `[Network]`

| Key | Meaning | Default |
| --- | --- | --- |
| `ServerEndpoint` | Server URL starting with `http://` or `https://`, which the client connects to as `ws://` or `wss://` at `/ws` when the URL has no path; replaces `playerhub-server.txt`, which is no longer read | the public server |
| `AutoConnect` | Whether each game connects when it loads, which it does only with a `DisplayName` set; otherwise the settings window's Connect does | `true` |

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
| `Arrow`, `Bolt` | ammunition |

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
| `Arrow` | `IronArrow` |
| `Bolt` | `DLC1BoltSteel` (`Dawnguard.esm`) |

Vanilla helmets and hoods cover `Hair` and `Circlet`, not `Head`.

### `[Performance]`

| Key | Meaning | Default |
| --- | --- | --- |
| `ViewRadius` | Game units within which other players are shown, `0..100000`; sent in `Hello` and clamped by the server ([Interest and delivery](interest-and-delivery.md#subscriptions)); applies on the next connection. It is here rather than in `[Network]` because it bounds how many remote actors the client draws | `0`, the server's range, named `Auto` |
| `SpawnsPerSecond` | Remote actor and mount spawns started per second at most, `0..1000`; `0` is unlimited, named `Unlimited` ([Sync pipeline](sync-pipeline.md#actor-lifecycle)) | `20` |

### `[UI]`

| Key | Meaning | Default |
| --- | --- | --- |
| `SettingsKey` | Opens the settings window: `F1`–`F12`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, or `ScrollLock` | `F10` |
| `AccentColor` | Color of the theme's accent in the settings window and the chat, as `RRGGBB` ([UI overlay](ui-overlay.md#accent-color)) | `FF9000` |
| `BorderColor` | Color of the menu frame around the settings window and the open chat ([UI overlay](ui-overlay.md#menu-frame)): `Vanilla`, the grey of the game's menus, or `Accent`, the accent | `Vanilla` |
| `BorderThickness` | Thickness of that frame, lines and knots together, in percent of what the game draws around its message boxes, `50..100` | `70` |
| `StatusPanel` | Shows the status panel | `true` |
| `ItemPreview` | Shows the 3D item preview in item pickers | `true` |
| `CloseChatOnSend` | Closes the chat when Enter sends a line; when `false`, the chat stays open and Enter in an empty field does nothing ([UI overlay](ui-overlay.md#sending)) | `true` |
| `ChatWidth` | Width of the chat's lines in font sizes, `24..100`; set by dragging the open chat's right edge ([UI overlay](ui-overlay.md#size)) and not shown in the settings window | `30` |
| `ChatLines` | Lines of history the open chat shows, `9..40`; set by dragging its top edge, and not shown in the settings window | `10` |

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
| `AppearanceSync` | main loop | the next capture taking the new default race and sex, which publish a new revision for a modded race |
| `ConnectionMonitor` | main loop | reconnecting at once when the latest attempt went to another endpoint; reading `AutoConnect` and whether `DisplayName` is set when a game starts; ending the player's choice to connect when `DisplayName` is cleared |
| `ServerClient` | transport, worker | reading the endpoint for each attempt, and `DisplayName` and `ViewRadius` for each hello |
| `SettingsWindow` | overlay, input | reading `SettingsKey` on each focus key press |
| `StatusPanel`, item pickers | overlay | reading `StatusPanel` and `ItemPreview` each frame |
| `ChatInput` | overlay | reading `CloseChatOnSend` on each Enter that sends or finds the field empty |
| `Theme` | overlay | working out the accent and frame colors from `AccentColor` and `BorderColor` when the revision changes |
| `MenuFrame` | overlay | reading `BorderThickness` for each framed window each frame |
| `ChatSize` | overlay | reading `ChatWidth` and `ChatLines` each frame the chat draws, outside a drag |

## Saving

`Config::Apply` is called by the settings window, and by the chat when a drag
on its edges ends ([UI overlay](ui-overlay.md#size)).

- The current file is loaded with its comments and each setting written into it.
  SimpleIni keeps an existing key's comment and position, adds a new key with its
  description at the end of its section, and a missing section with its comment.
  Entries no setting reads are kept.
- A setting absent from the file and equal to its default stays absent, so a
  removed line keeps meaning "default".
- A missing or unreadable file is recreated from the commented default. A file
  whose existence cannot be checked is left alone and the defaults are used,
  since writing could truncate it.
- One lock serializes file access and `Apply`, so the live configuration and
  the file always change in the same order.
- The text goes to `playerhub.ini.tmp` and is renamed over the file, so a failed
  write never truncates it. A failed save is logged and reported in the window;
  the applied settings still last until restart.

`Config::GetDirectory` is the directory of `playerhub.ini`, for the plugin's
other files: the session tokens are kept there in `playerhub.session`, outside
this file ([Network](network.md#session)), the muted and vanished players in
`playerhub.players` ([Network](network.md#player-filters)), and the settings
window's fonts and
the menu frame's shape are read from `playerhub/fonts/` and `playerhub/frame/`
under it ([UI overlay](ui-overlay.md#themed-faces)).

## Acceptance criteria

1. Without a file, the first launch writes `playerhub.ini` next to the DLL with
   the defaults; later launches never overwrite it.
2. Invalid values, unknown keys, and configured forms that do not resolve or are
   not base-game items are logged and fall back to their defaults, or to no piece
   when the default is unusable too.
3. `[Network] ServerEndpoint` sets the server URL and `[General] DisplayName`
   the name peers see; the default file has an empty `DisplayName`, and the
   plugin stays disconnected until one is entered and Connect is pressed.
   `[Performance] SpawnsPerSecond` bounds spawns, so many players arriving at
   once do not spawn in one frame.
4. Applying changes takes effect without a restart, and the file keeps
   hand-written comments.
