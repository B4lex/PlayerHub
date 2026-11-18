# Configuration

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

Each setting is described once in `SettingsSchema.cpp` (`Config::Describe`),
through a visitor that reads the file, writes it, and draws the settings
window; a new option is a field in `Config::PluginConfig` plus one line there.
Sections are described in the order Connection, DefaultLook, UI, Controls,
Gameplay, Voice, Advanced, which orders a new file and the settings window's
sidebar ([UI overlay](ui-overlay.md#settings-window)).

- Every shown setting takes the `Text::Id` of its label after its key, ignored
  by reading and saving, so a label changes without renaming the key; one
  without a label does not compile. The default look's slots and weapon types take theirs
  from `kArmorSlotLabels` and `kWeaponTypeLabels`. Section titles, comments,
  and group comments are `Text::Id`s too: the window shows them in the game's
  language ([Localization](ui-overlay.md#localization)) and the file gets
  their English text (`Text::GetEnglish`).
- `Choice` takes the values the file stores and the `Text::Id`s the window
  shows for them.
- `Language` takes a translation's code, or empty for the game's language.
  Reading checks only that it is a code (`Text::IsLanguageCode`), since
  translations load later; `Text::Select` warns of a code without a file.
- `Config::SettingsVisitor::Form` takes the setting's item filter
  ([Appearance and equipment](appearance-and-equipment.md#item-filter-and-catalog)).
- `Unsigned` takes a maximum (a larger file value is invalid), an optional
  `Text::Id` naming zero for the window (the file stores `0`), and a slider
  step; the file takes any value up to the maximum, off the step too.
- `Percent` takes both limits and a step; a file value outside the limits or
  off the steps is invalid. Stored without a sign.
- `FinePercent` takes a maximum: any finite number from 0 to it
  (`ParseFloat`), stored as the shortest text that reads back as the same
  `float`. The window's slider moves in whole percents; a drag on the open
  chat stores a fraction, so the chat stays on its pixel.
- `Color` is `0xRRGGBB`, stored as six upper-case hexadecimal digits
  (`FormatColor`); reading also takes lower case and a leading `#`
  (`ParseColor`). It takes the swatches the window offers.
- `Layout` is a window measure the player sets by dragging that window: an
  unsigned value within both limits, not shown in the window, with a plain
  English comment for the file.
- `ShowNextIf` makes the next setting's row depend on a value described before
  it; reading and saving ignore it.
- `Comment` names a group of the settings after it: a comment line in a new
  file, a group heading in the window.
- `AudioDevice` is a device's system name, read and saved as a string
  (`SettingsVisitor::IsValidAudioDeviceName`: at most 255 bytes without a line
  break or `;`); it takes whether the device captures, and the window offers
  the devices it finds. `InputMeter` holds no value: the window draws the
  microphone's level against the threshold it takes.

## Settings

### `[Connection]`

Who the player is to peers and which server they join, in one section so the
connection button and every setting it depends on share one page
([UI overlay](ui-overlay.md#connection-controls)).

| Key | Meaning | Default |
| --- | --- | --- |
| `DisplayName` | The player's name as peers see it ([Appearance and equipment](appearance-and-equipment.md#display-name)): 3..15 ASCII letters, digits, and underscores (`Multiplayer::IsValidDisplayName`, [Network](network.md#sessions)); a file value breaking the rule loads as not set. Without one no connection opens. It is the session's display name, so the server refuses one another player holds; it reaches the server only when connecting, and the settings window locks it while a connection is wanted or open | empty |
| `ConnectAutomatically` | Whether each game connects when it loads, which it does only with a `DisplayName` set; otherwise the settings window's connection button does | `true` |
| `CustomServer` | Whether the client connects to `ServerAddress` instead of the [official server](#official-and-custom-server) | `false` |
| `ServerAddress` | The custom server's address, `host` or `host:port` ([Server address](#server-address)); empty is valid and means none. Read only while `CustomServer` is `true`, and kept while it is `false` | empty |
| `ServerTls` | Whether the client connects to `ServerAddress` over TLS (`wss://`) rather than plain (`ws://`). Read only while `CustomServer` is `true` | `false` |

#### Official and custom server

Goal: players connect to the official server bundled with the DLL without
seeing or entering its address, and only those who switch `CustomServer` on
enter one. Non-goal: keeping the official address secret; it is a plain string
in the DLL and, as a WebSocket URL, the key of its lines in
`PlayerHub.session` and `PlayerHub.players` ([Network](network.md#client)).

`Config::GetServerAddress` is the `Config::ServerAddress` (host, port, TLS)
the client connects to: the build's default while `CustomServer` is `false`,
`ServerAddress` with `ServerTls` while `true`, and none when `ServerAddress` is
empty, so nothing connects ([Network](network.md#connection)).
`Config::HasServerAddress` answers the same without building one, for per-frame
checks; `Config::GetDefaultServerAddress` is the build's default. The default
is never written to the file, so a later build with another official server
moves every player who did not switch, and the settings window never draws it.
The log calls it `the official server` and any other `a custom server`, never
by address.

The repository does not name the public server. The `server_address` and
`server_tls` xmake options hold it (`xmake f --server_address=example.org:443
--server_tls=y`, or an IDE profile's configure arguments), passed by the
target's `on_config` as the `PLAYERHUB_DEFAULT_SERVER_ADDRESS` and
`PLAYERHUB_DEFAULT_SERVER_TLS` defines. Options, not environment variables,
because an IDE's run configuration does not reach the build. Release builds set
them; the defaults, `127.0.0.1` without TLS, reach a server run from `server/`
on the same machine. An invalid `server_address` fails the configuration and a
`static_assert` in `SettingsSchema.cpp`.

#### Server address

`ServerAddress` and `server_address` take the same form
(`ParseServerAddress` in `SettingsSchema.cpp`, mirrored in `xmake.lua`):

- a host name of ASCII letters, digits, `-`, and `.`, or an IPv4 address; at
  most 253 characters;
- optionally `:` and a port of 1 to 5 digits within `1..65535`; without one
  the port is `14201`, the server's default `listen` port.

A scheme, path, query, user, whitespace, or an IPv6 literal (which would need
brackets) is invalid; a host name resolving to IPv6 works. TLS is the separate
`ServerTls` or `server_tls`, since the address cannot tell a TLS server from a
plain one and a TLS-first fallback could be downgraded by anyone on the path;
it is off by default because a default server serves plain WebSocket. The
address is the WebSocket's host, the voice relay's host when the server names
none ([Network](network.md#voice)), and part of the WebSocket URL that keys the
server's lines in the client's files ([Network](network.md#transport)).

### `[DefaultLook]`

What peers show in place of this player's race and items they cannot resolve,
or in place of all of them with `AlwaysUse`
([Appearance and equipment](appearance-and-equipment.md#default-look)). Form
values are `Plugin|FormID` with the plugin's local hexadecimal ID, as in
`Skyrim.esm|0x012E49`; a load-order form ID is rejected, since it differs
between modpacks, and a malformed value keeps the default. `Head`, `Circlet`,
and `Backpack` may be empty, setting no piece; other keys must have a value.
Only base-game forms are accepted: the five Special Edition masters and
`ccFSVSSE001-Backpacks.esl`, so a peer without the Anniversary upgrade resolves
every piece but the backpack.

| Key | Meaning |
| --- | --- |
| `AlwaysUse` | boolean, default `false`: peers show the configured race and items for every worn item, not only for those they cannot resolve |
| `Race` | a playable race; base-game plugins define only the 10 vanilla ones |
| `<Slot>` | armor for that biped slot; `Head` also stands in for `Hair` |
| `OneHandSword` … `Crossbow` | the weapon for that Creation Kit weapon type |
| `Arrow`, `Bolt` | ammunition |

Every biped slot is synchronized, but only the most visible have a key:
`Head`, `Body`, `Hands`, `Amulet`, `Feet`, `Shield`, `Circlet`, and `Backpack`
(slot 47, used by `ccFSVSSE001-Backpacks.esl` and most mods). A modded item
covering none of these, such as a ring, is left off.

Defaults, from `Skyrim.esm` unless noted: `Race` `NordRace`.

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

Four groups: Display (`Language`, `Scale`, `AccentColor`, `BorderColor`,
`BorderThickness`, `BackgroundOpacity`), Players (`Nameplates`, `NameplateDistance`), Chat
(`CloseChatOnSend`, `ChatWidth`, `ChatLines`, `ChatPositionHorizontal`,
`ChatPositionVertical`), and Items (`ItemPreview`).

| Key | Meaning | Default |
| --- | --- | --- |
| `Language` | Language of the overlay's text and notices: `en`, or the code of a translation in `PlayerHub/locale/`, its file name without `.po` (`de`, `zh_TW`), in any case; empty follows the game's language. A value that is not a code warns and keeps the default; a code without a file uses the game's language ([UI overlay](ui-overlay.md#localization)) | empty |
| `Scale` | Size of the overlay and its text in percent, `80..130` in steps of `5`; the windows laid out in font sizes grow and shrink with it ([UI overlay](ui-overlay.md#scale)) | `100` |
| `AccentColor` | Color of the theme's accent in the settings window and the chat, as `RRGGBB` ([UI overlay](ui-overlay.md#accent-color)) | `FF9000` |
| `BorderColor` | Color of the menu frame around the settings window and the open chat ([UI overlay](ui-overlay.md#menu-frame)): `Vanilla`, the grey of the game's menus, or `Accent`, the accent | `Vanilla` |
| `BorderThickness` | Thickness of that frame, lines and knots together, in percent of what the game draws around its message boxes, `30..100` | `50` |
| `BackgroundOpacity` | Opacity of the background of the overlay's windows, popups, prompts, and notices in percent, `25..100` in steps of `5`; below `100` the game shows blurred behind them ([UI overlay](ui-overlay.md#background-blur)) | `75` |
| `Nameplates` | Shows other players' display names above their heads while in sight and not under the crosshair ([UI overlay](ui-overlay.md#nameplates)) | `true` |
| `NameplateDistance` | Game units from the player within which nameplates show, `0..4000`, about 70 to a meter; `0` shows none and skips their line-of-sight tests, as `Nameplates = false` does | `2000` |
| `CloseChatOnSend` | Closes the chat when Enter sends a line; when `false`, the chat stays open and Enter in an empty field does nothing ([UI overlay](ui-overlay.md#sending)) | `true` |
| `ItemPreview` | Shows the 3D item model in item pickers and the trade window's preview column, and item thumbnails in the trade window's offers; off, the cells show names and the column only the item card | `true` |
| `ChatWidth` | Width of the chat's lines in font sizes, `24..100`; set by dragging the open chat's left or right edge ([UI overlay](ui-overlay.md#size)) and not shown in the settings window | `30` |
| `ChatLines` | Lines of history the open chat shows, `9..40`; set by dragging its top or bottom edge, and not shown in the settings window | `15` |
| `ChatPositionHorizontal` | Distance of the open chat's left edge from the screen's left edge in percent of the screen's width, `0..100` with any fraction; the chat stays there and its size is limited to the room left on the screen; also set by dragging the open chat or its left edge ([UI overlay](ui-overlay.md#position)) | `1` |
| `ChatPositionVertical` | Distance of the open chat's bottom edge from the screen's bottom edge in percent of the screen's height, `0..100` with any fraction; the default keeps it above the magicka bar; also set by dragging the open chat or its bottom edge | `8` |

### `[Controls]`

Keyboard keys, each a DirectInput scan code stored by name
(`Config::FormatKey`, `ParseKey`, case-insensitive). The settings window binds
them by a key press ([UI overlay](ui-overlay.md#key-bindings)) and shows each
key's US-layout label (`FormatKeyLabel`), word keys such as Space in the game's
language.

| Key | Meaning | Default |
| --- | --- | --- |
| `OpenSettings` | Opens and closes the settings window | `F4` |
| `OpenChat` | Opens the chat ([UI overlay](ui-overlay.md#window-and-input)) | `Enter` |
| `ManagePlayers` | Opens the [player manager](ui-overlay.md#player-manager) alone, without the chat | `O` |
| `AcceptTrade` | Accepts another player's trade invitation while it waits for an answer ([Trade](trade.md#invitation)). It is the game's only while no invitation waits | `Y` |
| `DeclineTrade` | Declines another player's trade invitation, or withdraws the player's own, while it waits | `N` |
| `Microphone` | Held to be heard with push-to-talk; unused with voice activation ([Voice chat](voice-chat.md#activation)). The game keeps the key's own action | `V` |

- A key is `F1`–`F12`, a letter `A`–`Z`, a digit `0`–`9`, `Minus`,
  `Equals`, `LeftBracket`, `RightBracket`, `Semicolon`, `Apostrophe`,
  `Backslash`, `IntlBackslash` (the ISO key beside left Shift, scan code
  `0x56`), `Comma`, `Period`, `Slash`, `Space`, `Enter`, `CapsLock`, `Insert`,
  `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`,
  `Right`, `Pause`, `ScrollLock`, `NumLock`, `Numpad0`–`Numpad9`,
  `NumpadDecimal`, `NumpadPlus`, `NumpadMinus`, `NumpadMultiply`,
  `NumpadDivide`, or `NumpadEnter`. Enter and keypad Enter can be bound to
  any action; inside a focused window they still reach Dear ImGui.
- Reserved keys have no name, so neither the file nor the window takes them:
  Escape (cancels and closes), Tab (the chat's), the console key left of `1`,
  Backspace and the modifiers (text editing and shortcuts; Backspace also
  clears a binding in the window), and the Windows, context-menu, and Print
  Screen keys.
- An empty value leaves `ManagePlayers`, `AcceptTrade`, `DeclineTrade`, or
  `Microphone` unbound (scan code 0, stored as `AcceptTrade =`); the prompt
  leaves an unbound key out. An empty `OpenSettings` is `F4` and an empty
  `OpenChat` is `Enter`, since each is the only way to open its window.
  Backspace in the window sets the same values.
- Two settings may hold the same key in the file; the window does not let the
  player bind one key twice. Unbound actions do not conflict.

### `[Gameplay]`

How other players' actions show and how far they reach into the local world,
in two groups: Effects (`ShowSpellEffects`, `ShowShoutEffects`) and Isolation
(`IsolateSpellEffects`, `IsolateShoutEffects`, `DisableCollision`). Each
applies to what this client shows of remote actors. Remote weapon attacks
always hit nothing ([Remote actor](remote-actor.md#combat-isolation)).
`GameplayPolicy` mirrors the section for the hooks that read it.

| Key | Meaning | Default |
| --- | --- | --- |
| `ShowSpellEffects` | Remote hands may hold the receiver's own spell, and remote staves cast the receiver's own enchantment ([Combat presentation](combat-presentation.md#staves)), whose fire shows natively; off, every hand holds or casts a stand-in, which shows the cast and the spell's look in hand only ([Combat presentation](combat-presentation.md#spells)). It also governs the lasting effects shown on remote players from their active effects ([Combat presentation](combat-presentation.md#active-effects)) | `true` |
| `ShowShoutEffects` | Remote shouts and powers fire their spells; off, a shout still charges, speaks its words, and plays its release, without its breath, wave, projectile, or self effects, and a power casts nothing ([Combat presentation](combat-presentation.md#shout-and-power-receiver)) | `true` |
| `IsolateSpellEffects` | Remote spells act on nothing; off, they act on the local world as a local NPC's would ([Combat presentation](combat-presentation.md#isolation-settings)) | `true` |
| `IsolateShoutEffects` | The same for remote shouts and powers | `true` |
| `DisableCollision` | The local player passes through remote actors; off, a network-placed remote actor or mount collides as a local NPC does, with the local player, NPCs, projectiles, clutter, traps, and trigger volumes ([Remote actor](remote-actor.md#solid-remote-actors)); weapon hits between them still do not land | `true` |

### `[Voice]`

[Voice chat](voice-chat.md) with the players nearby, in three
groups: Microphone (`Mode`, the input meter, `ActivationLevel`,
`InputDevice`, `InputVolume`, `NoiseSuppression`), Other players
(`OutputDevice`, `OutputVolume`), and Indicator (`Indicator`,
`IndicatorPositionHorizontal`, `IndicatorPositionVertical`).

| Key | Meaning | Default |
| --- | --- | --- |
| `Mode` | `PushToTalk`, heard while `[Controls] Microphone` is held, or `VoiceActivation`, heard while speaking ([Voice chat](voice-chat.md#activation)) | `PushToTalk` |
| `ActivationLevel` | The input level at which voice activation opens, `0..100` on the meter's scale, 0 at -60 dBFS and 100 at full scale; the settings window shows it only with `VoiceActivation` | `35` |
| `InputDevice`, `OutputDevice` | The capture and playback devices by their system names, as the settings window lists them; empty is the system's default device. A name no device has logs a warning and uses the default | empty |
| `InputVolume` | How loud the microphone is sent, `50..150` percent in steps of `5`; the gain is 1.4 times its square ([Voice chat](voice-chat.md#capture-and-encoding)) | `100` |
| `NoiseSuppression` | Runs the microphone through RNNoise before encoding, and lets voice activation open only on speech | `true` |
| `OutputVolume` | Gain on other players' voices in percent, `0..200` in steps of `5` | `100` |
| `Indicator` | Shows the microphone on the screen while the player is heard ([UI overlay](ui-overlay.md#voice-chat)) | `true` |
| `IndicatorPositionHorizontal` | Distance of the microphone's left edge from the screen's left edge in percent of the screen's width, `0..100` with any fraction; the microphone stays wholly on the screen. The settings window shows it only with `Indicator` on | `1` |
| `IndicatorPositionVertical` | Distance of the microphone's bottom edge from the screen's bottom edge in percent of the screen's height, `0..100` with any fraction; the default is under the chat's default place. Shown as the horizontal position is | `5` |

### `[Advanced]`

Settings most players leave alone, in two groups: Performance, then
Diagnostics. The maximums are narrow so the sliders stay usable: a server
clamps `ViewRadius` to its enter radius (8192 by default), so 15000 leaves room
for a larger one. A file value above a maximum loads as the default.

| Key | Meaning | Default |
| --- | --- | --- |
| `ViewRadius` | Game units within which other players are shown, `0..15000`, slider step `100`; sent in `Hello` and clamped by the server ([Interest and delivery](interest-and-delivery.md#subscriptions)); applies on the next connection | `0`, the server's range |
| `SpawnsPerSecond` | Remote actor and mount spawns started per second at most, `0..100`; `0` is unlimited and shown by the schema's zero name ([Sync pipeline](sync-pipeline.md#actor-lifecycle)) | `20` |
| `LightsLimit` | Lights on remote actors that light the scene at most, nearest to the camera first, `0..8` (`AdvancedSettings::kMaximumRemoteLights`); `0` leaves every one dark and has no zero name; the effects stay ([Remote actor](remote-actor.md#light-budget)) | `0` |
| `StatusPanel` | Shows the [status panel](ui-overlay.md#status-panel), a debugging readout | `false` |

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
| `RemoteLights` | main loop | reading `LightsLimit` in the first pass after the revision changes |
| `GameplayPolicy` | main loop | reading `[Gameplay]` first in `SyncCoordinator::OnFrame` after the revision changes, into atomics the isolation hooks read on any thread |
| `EquipmentSync` hands | main loop | resolving a remote actor's hands again when `ShowSpellEffects` differs from the value they were resolved with |
| `DefaultLook` | main loop | `Refresh`, first in `SyncCoordinator::OnFrame`, resolving every item again when `[DefaultLook]` differs |
| `EquipmentSync` | main loop | the next capture selecting from the new outfit and publishing a new revision |
| `AppearanceSync` | main loop | the next capture taking the new default race, which publishes a new revision for a modded race |
| `ConnectionMonitor` | main loop | reading `ConnectAutomatically` and whether `DisplayName` and a server address are set when a game starts; ending the player's choice to connect when `DisplayName` is cleared, or while there is no address and no connection is welcomed |
| `ServerClient` | transport, worker | reading `Config::GetServerAddress` for each attempt, and `DisplayName` and `ViewRadius` for each hello |
| `ConnectionControls` | overlay | reading `Config::HasServerAddress` each frame the `[Connection]` page draws |
| `SettingsWindow` | overlay, input | reading `OpenSettings` on each focus key press |
| `ChatWindow` | input | reading `OpenChat` and `ManagePlayers` on each focus key press |
| `TradePrompt` | overlay, input | reading `AcceptTrade` and `DeclineTrade` on each focus key press and each frame it draws |
| `VoiceChatSync` | main loop | reading `[Voice]` and `[Controls] Microphone` every frame in `Maintain`, and configuring the audio engine when they differ |
| `StatusPanel`, item pickers, trade window | overlay | reading `[Advanced] StatusPanel` and `[UI] ItemPreview` each frame |
| `ChatInput` | overlay | reading `CloseChatOnSend` on each Enter that sends or finds the field empty |
| `Theme` | overlay | working out the accent, frame, and background colors from `AccentColor`, `BorderColor`, and `BackgroundOpacity` when the revision changes |
| `MenuFrame` | overlay | reading `BorderThickness` for each framed window each frame |
| `Overlay` | overlay | selecting the text language from `Language` before the frame when the revision changes, and reloading the fonts when it changes the Han font order |
| `ChatSize` | overlay | reading `ChatWidth` and `ChatLines` each frame the chat draws, outside a drag |
| `ChatPosition` | overlay | reading `ChatPositionHorizontal` and `ChatPositionVertical` each frame the chat or its recent lines draw, outside a drag |
| `VoiceIndicator` | overlay | reading `[Voice] Indicator` and its two positions each frame the player is heard |

## Saving

`Config::Apply` is called by the settings window, and by the chat when a drag
on its edges ([UI overlay](ui-overlay.md#size)) or one that moves it
([UI overlay](ui-overlay.md#position)) ends.

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

`Config::GetDirectory` is the directory of `PlayerHub.ini`, which also holds
`PlayerHub.session` ([Network](network.md#session)), `PlayerHub.players`
([Network](network.md#player-filters)), and the `PlayerHub/fonts/`,
`PlayerHub/frame/`, and `PlayerHub/locale/` assets
([UI overlay](ui-overlay.md#themed-faces)).

## Acceptance criteria

1. Without a file, the first launch writes `PlayerHub.ini` next to the DLL with
   the defaults; later launches never overwrite it.
2. Invalid values, unknown keys, and configured forms that do not resolve or are
   not base-game items are logged and fall back to their defaults, or to no piece
   when the default is unusable too.
3. The default file connects to the official server without naming it;
   `CustomServer = true` connects to `ServerAddress` (over TLS with
   `ServerTls = true`) instead, and with an empty one nothing connects. The
   default `DisplayName` is empty, and the plugin stays disconnected until one
   is entered and Connect is pressed.
4. `[Advanced] SpawnsPerSecond` bounds spawns, so many players arriving at once
   do not spawn in one frame.
5. Applying changes takes effect without a restart, and the file keeps
   hand-written comments.

## Known limitations

- Sections and keys are not migrated when they are renamed or moved: a
  file's lines under a name no setting reads are logged as unknown and the
  defaults apply, so their values are entered again.
