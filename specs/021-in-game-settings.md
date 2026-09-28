# In-Game Settings Window

## Status

Implemented, pending a Windows build and in-game check. On Linux,
`SettingsSchema.cpp` and `PluginConfig.cpp` were compiled against SimpleIni
with stubbed SKSE headers and checked for saving: comments and unknown keys are
kept, and a missing file is recreated
from the commented default. `SettingsWindow.cpp` was run for several frames in
a headless Dear ImGui 1.92.9 context, with every tab drawn and simulated mouse
and keyboard input: an invalid endpoint blocks Apply, and a valid one is applied
and saved. The mouse path of `InputCapture`, the reconnect, and equipment
re-resolution were not run.

## Goal

Players change `skyrim-mp.ini` (spec 019) in game and see the result at once:
a new server endpoint reconnects, a new default outfit re-dresses the local
player on peers, and a new spawn rate applies on the next pass.

## Non-goals

- Pausing the game. The window captures input like the chat (spec 020); the
  world keeps running and peers keep seeing the player.
- Watching the file for external edits. Those still apply on restart.

## Opening and closing

- `[UI] SettingsKey` opens the window during gameplay, as a window focus key
  (spec 010). The key is one of `F1`–`F12`, `Insert`, `Delete`, `Home`, `End`,
  `PageUp`, `PageDown`, `Pause`, or `ScrollLock`, and defaults to `F10`. A
  changed key applies as soon as it is applied.
- The title bar's close button, the Close button, or Escape closes it. Escape
  first goes to an active text field or an open combo. Any menu taking over
  input also closes it (spec 010).
- Every opening starts from the live configuration; closing discards what was
  not applied.

## Window

- Centered, 44 × 32 font sizes, one tab per file section. Only a tab's content
  scrolls; the buttons stay below it.
- The widgets come from the same `Config::Describe` visitor that reads and
  writes the file, so a new setting appears without UI code: a checkbox for a
  bool, a clamped number field for an unsigned value, a text field for a string,
  an item picker for a form (spec 022), and a combo for a choice or a key. Section and group comments are shown as notes.
- Text that does not parse turns red and leaves the setting at its last valid
  value. Apply stays disabled while any field is red.
- Apply is enabled when the edited settings differ from the live ones. Revert
  returns to the live settings and Defaults to the built-in defaults, without
  applying. The footer reports unapplied changes, a successful save, or a
  failed one.
- The window uses the mouse (`Window::UsesMouse`); see spec 010 for the cursor.

## Server connection

The `[Network]` tab's `ServerEndpoint` row ends with `ConnectionControls`, on
the same line as the text field:

- An indicator circle: green while the client is connected, red otherwise.
  Hovering it shows `Connected, ping N ms` (`Connected` before the first
  sample), `Connecting...`, `Disconnecting...`, or `Disconnected`.
- A button that follows `Network::GetConnectionState` (spec 010), so its label
  never claims a state the transport has not reached. It keeps its width and
  identity when the label changes.

  | State | Button | Press |
  | --- | --- | --- |
  | Connected | `Disconnect` | stops wanting a connection |
  | Connecting | `Cancel` | stops wanting a connection |
  | Disconnecting | `Disconnecting...`, disabled | — |
  | Disconnected | `Connect` | wants a connection again |

  Not wanting a connection closes it and removes every remote player until
  Connect; see spec 010 for when the client connects.
- The button acts on the live endpoint. An edited endpoint has to be applied
  first, and applying one while connected moves the connection to it.

## Configuration changes

`Config::Get()` returns a `shared_ptr` to an immutable snapshot, stored in an
`std::atomic<std::shared_ptr>`. `Config::Apply` publishes a new snapshot, then
advances `Config::GetRevision()`, then saves the file. A consumer compares the
revision with the one it last saw and only then looks at the snapshot, so an
unchanged configuration costs one atomic load per pass. A consumer that reads the
revision before taking a snapshot never misses a change.

| Consumer | Thread | Follows a change by |
| --- | --- | --- |
| `ActorLifecycle` | main loop | reading `SpawnsPerSecond` once per pass in `RefillSpawnAllowance` |
| `DefaultOutfit` | main loop | `Refresh`, run first in `SyncCoordinator::OnFrame`: resolves every item again when `[DefaultOutfit]` differs |
| `EquipmentSync` | main loop | the next local capture selecting from the new outfit, which differs from the published equipment and so publishes a new revision |
| `AppearanceSync` | main loop | the next local capture taking the new default race and sex, which, for a modded race, differs from the published appearance and so publishes a new revision |
| `ConnectionMonitor` | main loop | when the latest attempt went to another endpoint: abandoning a live connection and starting a reconnect at once, with the attempt count reset |
| `SocketIOClient` | transport, worker | reading the endpoint for each attempt; logs name the attempt's endpoint |
| `SettingsWindow` | overlay, input | reading `SettingsKey` on each focus key press |
| `ConnectionMonitor` | main loop | reading `AutoConnect` when a game starts; a change applies from the next game |
| `StatusPanel` | overlay | reading `StatusPanel` on each frame |

## Saving

- The current file is loaded with its comments, and each setting is written
  into it: SimpleIni keeps an existing key's comment and position, adds a new
  key with its description comment at the end of its section, and a missing
  section with its comment. Entries no setting reads are kept.
- A setting that is absent from the file and equal to its default stays absent,
  so a removed line keeps meaning "default".
- A missing or unreadable file is recreated from the commented default file.
- The text goes to `skyrim-mp.ini.tmp` and is renamed over the file, so a failed
  write never truncates it. A failed save is logged and reported in the window;
  the applied settings still last until restart.

## Verification

- Linux (done): the config round trip and headless window checks in Status.
- In game (pending): F10 opens the window with a cursor, and the game receives no
  input until it closes. Changing `SpawnsPerSecond` takes effect without a
  restart. Picking or resetting a default outfit piece dresses the local
  player with it on peers. A new endpoint disconnects, removes remote players,
  and connects to the new server; the status panel follows. The indicator
  turns green once connected and red on Disconnect, which removes remote
  players and keeps them away through a save load until Connect. Pressing
  Connect right after Disconnect shows `Cancel` until the connection opens,
  about 3 seconds later, and never `Disconnect` while disconnected. A new
  `SettingsKey` opens the window and the old one no longer does. The file keeps
  hand-written comments after Apply. Chat still opens with Enter and gets no
  cursor.
