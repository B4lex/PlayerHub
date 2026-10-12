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
`config/PluginFiles`, which now reads and replaces the file, passed a Linux
syntax check only; the stubbed checks above predate it.
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
The `server_endpoint` xmake option is not yet built on Windows. On Linux,
xmake 3.0.6 ran the same option and `on_config` in a scratch project: the
define holds `http://127.0.0.1:3000` by default and the configured value after
`xmake f --server_endpoint=...`; changing it recompiles and relinks without
`-r`; `ftp://bad`, a value with a space or a quote, and an empty value fail
`xmake f`; the option is listed in `xmake f --help` with its default; a rule's
`on_config` still runs beside the target's. The project's own `xmake.lua`
was not loaded on Linux.
`[Network] CustomServer` is not yet built on Windows or run in game. On
Linux the same two files ran with the settings window's harness
([UI overlay](ui-overlay.md#status)): the default is `false`, which resolves
to the build's default endpoint, and switching it on resolves to
`ServerEndpoint`, empty until a URL is entered, and off again to the default
with the URL kept. The file checks below ran with the earlier, inverted
`ConnectToOfficialServer` key, which `CustomServer` replaced without other
changes to loading or saving, and `ConnectionMonitor.cpp`, built with the
default endpoint `https://official.example`: the default file has the switch
and an empty `ServerEndpoint` under their comments, before `AutoConnect`, and
no trace of the default URL; a file with only
`ServerEndpoint = http://127.0.0.1:3000` loads without a warning as the
official server with that URL kept; the custom server connects to
`ServerEndpoint`, an empty one loads without a warning as no endpoint, and
`ftp://bad` warns and keeps it empty; applying the switch and a URL adds both
lines under a hand-written comment, and switching back keeps the URL.
`[Controls]` is not yet built on Windows or run in game. On Linux the same
two files and `PluginFiles.cpp` were compiled against SimpleIni with stubs:
the default file ends with `[Controls]` and `OpenSettings = F10`,
`AcceptTrade = Y`, and `DeclineTrade = N` under their comments, after
`[UI]`, which has no key; `numpad3` loads as `Numpad3`; `Escape` warns and
keeps `Y`; `SettingsKey` and `TradeAcceptKey` lines in `[UI]` are logged as
unknown and ignored; applying a key adds its line, which loads back; an
empty `DeclineTrade` loads as unbound and an empty `OpenSettings` as `F10`,
and applying an unbound `AcceptTrade` saves `AcceptTrade =`, which loads
back unbound. Every
key name parses back to its scan code, and none of the reserved keys has a
name. Run again with `OpenChat` and `ManagePlayers` (2026-10-11): the
default file has `OpenChat = Enter` and `ManagePlayers = F11` between
`OpenSettings` and `AcceptTrade`; `numpadenter` loads as `NumpadEnter`; an
empty `OpenChat` loads as `Enter`; applying an unbound `ManagePlayers` saves
`ManagePlayers =`, which loads back unbound. The `ManagePlayers` default
changed to `O` afterwards (2026-10-11) without a rerun. The section order
changed afterwards too (2026-10-11), so `[Controls]` now follows `[UI]` before
`[Network]`; `SettingsSchema.cpp` passed the g++ syntax check only.
`[UI] CloseChatOnSend` is not yet built on Windows or run in game. On Linux
the same two files ran with the settings window's harness: the default file
has `CloseChatOnSend = true` under its comment, between `StatusPanel` and
`ChatWidth`; `false` loads; `maybe` warns and keeps `true`; a file without
the key loads as `true`; switching it off and on in the window saves each
value. `ItemPreview` and `StatusPanel` moved afterwards (2026-10-12) to follow
`CloseChatOnSend`, last among the shown `[UI]` keys; `SettingsSchema.cpp` was
not rechecked.

## Scope

`PlayerHub.ini` is the plugin's general configuration. This system owns the
file, its schema, the immutable settings snapshot, how consumers follow a
change, and saving. The settings window that edits it is in
[UI overlay](ui-overlay.md#settings-window); what each value does is in its
consumer's system.

## Components

| Module | Responsibility |
| --- | --- |
| `config/PluginConfig` | `Config::PluginConfig`, `Load`, `Get`, `GetRevision`, `Apply`, saving |
| `config/SettingsSchema` | `Config::Describe`: every setting described once, through a visitor |
| `config/PluginFiles` | Reading, line iteration, and atomic replacement of the plugin's files, shared with the session store, player filters, and menu frame |

## File

`PlayerHub.ini` sits in the directory of the loaded plugin DLL, found through
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
value in the file is invalid, and a step: a value that is not the minimum
plus whole steps is invalid too. The file stores the number without a sign.
`Color` is an opaque `0xRRGGBB` value that the file stores as six hexadecimal
digits (`FormatColor`, upper case); reading also takes lower case and a leading
`#` (`ParseColor`). It takes swatches, the colors the settings window offers.
`Layout` is a measure of a window that the player sets by dragging that window:
an unsigned value within both limits, read and saved as any other, which the
settings window does not show. `ShowNextIf` makes the next setting depend on a
value described before it; reading and saving ignore it, and the settings
window leaves the row out while the value is false.
`Config::Describe` describes the sections in the order General,
DefaultOutfit, UI, Controls, Network, Performance: the order of a new file's
sections and of the settings window's sidebar
([UI overlay](ui-overlay.md#settings-window)).

## Settings

### `[General]`

| Key | Meaning | Default |
| --- | --- | --- |
| `DisplayName` | The player's name as peers see it, in the player label over the actor and in chat ([Appearance and equipment](appearance-and-equipment.md#display-name)): 3..15 ASCII letters, digits, and underscores, as the server accepts (`Multiplayer::IsValidDisplayName`, [Network](network.md#sessions)). A file value that breaks the rule loads as not set. Empty means not set, and no connection is opened without one ([Network](network.md#connection)). It is the display name of the player's session, so the server refuses one another player holds ([Network](network.md#sessions)), and it reaches the server only when connecting: the settings window locks it while a connection is wanted or open ([UI overlay](ui-overlay.md#connection-controls)) | empty |

### `[DefaultOutfit]`

What peers show in place of this player's race and items they cannot resolve
([Appearance and equipment](appearance-and-equipment.md#default-outfit)). Form
values are `Plugin|FormID`, the defining plugin and its local hexadecimal ID, as
in `Skyrim.esm|0x012E49`. A full load-order form ID is rejected, since it differs
between modpacks. Every key has a value: an empty or malformed one is invalid and
keeps the default. Only base-game forms are accepted: the five Special Edition
masters and `ccFSVSSE001-Backpacks.esl`, without other Creation Club content,
so a peer on Special Edition or on Anniversary Edition without the upgrade
resolves every piece but the backpack.

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

### `[UI]`

| Key | Meaning | Default |
| --- | --- | --- |
| `FontScale` | Size of the overlay's text in percent, `80..130` in steps of `5`; the windows laid out in font sizes grow and shrink with it ([UI overlay](ui-overlay.md#font-scale)) | `100` |
| `AccentColor` | Color of the theme's accent in the settings window and the chat, as `RRGGBB` ([UI overlay](ui-overlay.md#accent-color)) | `FF9000` |
| `BorderColor` | Color of the menu frame around the settings window and the open chat ([UI overlay](ui-overlay.md#menu-frame)): `Vanilla`, the grey of the game's menus, or `Accent`, the accent | `Vanilla` |
| `BorderThickness` | Thickness of that frame, lines and knots together, in percent of what the game draws around its message boxes, `50..100` | `70` |
| `Nameplates` | Shows other players' display names above their heads while in sight ([UI overlay](ui-overlay.md#nameplates)) | `true` |
| `NameplateDistance` | Game units from the player within which nameplates show, `0..10000`, about 70 to a meter; `0` shows none and skips their line-of-sight tests, as `Nameplates = false` does; the settings window shows it only while `Nameplates` is on | `2100` |
| `CloseChatOnSend` | Closes the chat when Enter sends a line; when `false`, the chat stays open and Enter in an empty field does nothing ([UI overlay](ui-overlay.md#sending)) | `true` |
| `ItemPreview` | Shows the 3D item model in item pickers and the trade window's preview column, and item thumbnails in the trade window's offers; off, the cells show names and the column only the item card | `true` |
| `StatusPanel` | Shows the status panel | `false` |
| `ChatWidth` | Width of the chat's lines in font sizes, `24..100`; set by dragging the open chat's right edge ([UI overlay](ui-overlay.md#size)) and not shown in the settings window | `30` |
| `ChatLines` | Lines of history the open chat shows, `9..40`; set by dragging its top edge, and not shown in the settings window | `10` |

### `[Controls]`

Keyboard keys, each a DirectInput scan code stored by name
(`Config::FormatKey`, `ParseKey`, case-insensitive). The settings window
binds them by a key press ([UI overlay](ui-overlay.md#key-bindings)) and
shows each key's label (`FormatKeyLabel`), what a US layout prints on it:
`Num 7` for `Numpad7`, `[` for `LeftBracket`.

| Key | Meaning | Default |
| --- | --- | --- |
| `OpenSettings` | Opens and closes the settings window | `F10` |
| `OpenChat` | Opens the chat ([UI overlay](ui-overlay.md#window-and-input)) | `Enter` |
| `ManagePlayers` | Opens [Manage players](ui-overlay.md#manage-players) alone, without the chat | `O` |
| `AcceptTrade` | Accepts another player's trade invitation while it waits for an answer ([Trade](trade.md#invitation)). It is the game's only while no invitation waits | `Y` |
| `DeclineTrade` | Declines another player's trade invitation, or withdraws the player's own, while it waits | `N` |

- A key is `F1`–`F12`, a letter `A`–`Z`, a digit `0`–`9`, `Minus`,
  `Equals`, `LeftBracket`, `RightBracket`, `Semicolon`, `Apostrophe`,
  `Backslash`, `IntlBackslash` (the ISO key beside left Shift, scan code
  `0x56`), `Comma`, `Period`, `Slash`, `Space`, `Enter`, `CapsLock`, `Insert`,
  `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`,
  `Right`, `Pause`, `ScrollLock`, `NumLock`, `Numpad0`–`Numpad9`,
  `NumpadDecimal`, `NumpadPlus`, `NumpadMinus`, `NumpadMultiply`,
  `NumpadDivide`, or `NumpadEnter`. Enter and keypad Enter are open to
  every action now that the chat's key is one of them; inside a focused
  window they still reach Dear ImGui.
- Reserved keys have no name, so neither the file nor the window takes them:
  Escape cancels and closes, Tab is the chat's, the
  key left of `1` opens the console, Backspace and the modifiers (Shift,
  Ctrl, Alt) edit text and form shortcuts, Backspace also clears a binding
  in the window, and the Windows keys, the
  context-menu key, and Print Screen belong to the system.
- An empty value leaves `ManagePlayers`, `AcceptTrade`, or `DeclineTrade`
  unbound (scan code 0), and the file keeps it as `AcceptTrade =`; an
  unbound action has no key and the prompt leaves it out. An empty
  `OpenSettings` is `F10` and an empty `OpenChat` is `Enter`, since each key
  is the only way to open its window (a whisper from the
  [world player menu](ui-overlay.md#player-menu-in-the-world) aside). Backspace in the
  window sets the same values.
- Two settings may hold the same key in the file; the window does not let
  the player bind one key twice. Unbound actions do not conflict.
- The section replaced `[UI] SettingsKey`, `TradeAcceptKey`, and
  `TradeDeclineKey` without migrating them: an earlier file's lines are
  logged as unknown and the defaults apply.

### `[Network]`

| Key | Meaning | Default |
| --- | --- | --- |
| `CustomServer` | Whether the client connects to `ServerEndpoint` instead of the [official server](#official-and-custom-server). Replaces `ConnectToOfficialServer`, which is logged as unknown without migration, so a file that switched it off connects to the official server until `CustomServer` is switched on | `false` |
| `ServerEndpoint` | The custom server's URL, starting with `http://` or `https://`, which the client connects to as `ws://` or `wss://` at `/ws` when the URL has no path; empty is valid and means none. Read only while `CustomServer` is `true`, and kept while it is `false`. Replaces `playerhub-server.txt`, which is no longer read | empty |
| `AutoConnect` | Whether each game connects when it loads, which it does only with a `DisplayName` set; otherwise the settings window's Connect does | `true` |

#### Official and custom server

Goal: players connect to the official server bundled with the DLL without
seeing or entering its URL, and only those who switch `CustomServer` on enter
one. Non-goal: keeping the official URL
secret; it is a plain string in the DLL and the key of its lines in
`PlayerHub.session` and `PlayerHub.players` ([Network](network.md#client)).

`Config::GetServerEndpoint` is the URL the client connects to: the build's
default while `CustomServer` is `false`, `ServerEndpoint` while it is
`true`, and empty when that is empty, in which case nothing connects
([Network](network.md#connection)). `Config::GetDefaultServerEndpoint` is the
build's default. The default is never written to the file, so a later build
with another official server moves every player who did not switch, and the
settings window never draws it ([UI overlay](ui-overlay.md#settings-window)).
The log names it `the official server` instead of its URL
([Network](network.md#client)).

The repository does not name the public server. The `server_endpoint` xmake
option holds it, set with `xmake f --server_endpoint=https://...` or in an
IDE build profile's configure arguments, and kept in xmake's configuration
cache. The PlayerHub target's `on_config` passes it as the
`PLAYERHUB_DEFAULT_SERVER_ENDPOINT` string define, which `SettingsSchema.cpp`
holds as the default endpoint. It is an option rather than an environment
variable because an IDE's run configuration does not reach the build, and the
configured value is visible in `xmake f --menu`. Release builds set it to the
public server. Its default is `http://127.0.0.1:3000`, the local server's
default address, so a build without it reaches a server run from `server/` on
the same machine. A value that does not start with `http://` or `https://`, or
that holds whitespace, a quote, or a backslash, fails the configuration, since
the define is a C string literal and the client could not connect to it. xmake
tracks each file's flags, so a changed value recompiles the plugin. The value
is still a plain string in the DLL.

### `[Performance]`

| Key | Meaning | Default |
| --- | --- | --- |
| `ViewRadius` | Game units within which other players are shown, `0..100000`; sent in `Hello` and clamped by the server ([Interest and delivery](interest-and-delivery.md#subscriptions)); applies on the next connection. It is here rather than in `[Network]` because it bounds how many remote actors the client draws | `0`, the server's range, named `Auto` |
| `SpawnsPerSecond` | Remote actor and mount spawns started per second at most, `0..1000`; `0` is unlimited, named `Unlimited` ([Sync pipeline](sync-pipeline.md#actor-lifecycle)) | `20` |

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
| `ConnectionMonitor` | main loop | reading `AutoConnect` and whether `DisplayName` and an endpoint are set when a game starts; ending the player's choice to connect when `DisplayName` is cleared, or while the endpoint is empty and no connection is welcomed |
| `ServerClient` | transport, worker | reading `Config::GetServerEndpoint` for each attempt, and `DisplayName` and `ViewRadius` for each hello |
| `ConnectionControls` | overlay | reading whether `Config::GetServerEndpoint` is empty each frame the General section draws |
| `SettingsWindow` | overlay, input | reading `OpenSettings` on each focus key press |
| `ChatWindow` | input | reading `OpenChat` and `ManagePlayers` on each focus key press |
| `TradePrompt` | overlay, input | reading `AcceptTrade` and `DeclineTrade` on each focus key press and each frame it draws |
| `StatusPanel`, item pickers, trade window | overlay | reading `StatusPanel` and `ItemPreview` each frame |
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
- The text goes to `PlayerHub.ini.tmp` and is renamed over the file
  (`PluginFiles::ReplaceFile`), so a failed write never truncates it. A failed save is logged and reported in the window;
  the applied settings still last until restart.

`Config::GetDirectory` is the directory of `PlayerHub.ini`, for the plugin's
other files: the session tokens are kept there in `PlayerHub.session`, outside
this file ([Network](network.md#session)), the friends, muted, and vanished players in
`PlayerHub.players` ([Network](network.md#player-filters)), and the settings
window's fonts and
the menu frame's shape are read from `PlayerHub/fonts/` and `PlayerHub/frame/`
under it ([UI overlay](ui-overlay.md#themed-faces)).

## Acceptance criteria

1. Without a file, the first launch writes `PlayerHub.ini` next to the DLL with
   the defaults; later launches never overwrite it.
2. Invalid values, unknown keys, and configured forms that do not resolve or are
   not base-game items are logged and fall back to their defaults, or to no piece
   when the default is unusable too.
3. The default file connects to the official server without naming it;
   `[Network] CustomServer = true` connects to `ServerEndpoint`
   instead, and with an empty one nothing connects. `[General] DisplayName` sets
   the name peers see; the default file has an empty `DisplayName`, and the
   plugin stays disconnected until one is entered and Connect is pressed.
   `[Performance] SpawnsPerSecond` bounds spawns, so many players arriving at
   once do not spawn in one frame.
4. Applying changes takes effect without a restart, and the file keeps
   hand-written comments.
