# In-Game Settings Window

## Status

Implemented, pending a Windows build and in-game check. On Linux,
`SettingsSchema.cpp` and `PluginConfig.cpp` were compiled against SimpleIni
with stubbed SKSE headers and checked for saving: comments and unknown keys are
kept, a cleared default form is written empty, and a missing file is recreated
from the commented default. `SettingsWindow.cpp` was run for several frames in
a headless Dear ImGui 1.92.9 context, with every tab drawn and simulated mouse
and keyboard input: an invalid endpoint blocks Apply, and a valid one is applied
and saved. The mouse path of `InputCapture`, the reconnect, and equipment
re-resolution were not run.

## Goal

Players change `skyrim-mp.ini` (spec 019) in game and see the result at once:
a new server endpoint reconnects, new equipment fallbacks re-dress remote
players, and a new spawn rate applies on the next pass.

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
  an item picker for a form (spec 022), and a combo for a key. Section and group comments are shown as notes.
- Text that does not parse turns red and leaves the setting at its last valid
  value. Apply stays disabled while any field is red.
- Apply is enabled when the edited settings differ from the live ones. Revert
  returns to the live settings and Defaults to the built-in defaults, without
  applying. The footer reports unapplied changes, a successful save, or a
  failed one.
- The window uses the mouse (`Window::UsesMouse`); see spec 010 for the cursor.

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
| `EquipmentFallback` | main loop | `Refresh`, run first in `SyncCoordinator::OnFrame`: resolves every item again when `[EquipmentFallback]` differs and advances its generation |
| `EquipmentSync` | main loop | resolving remote equipment again when the fallback generation differs from the one it resolved with, which restarts reconciliation as a new revision would |
| `ConnectionMonitor` | main loop | when the latest attempt went to another endpoint: abandoning a live connection and starting a reconnect at once, with the attempt count reset |
| `SocketIOClient` | transport, worker | reading the endpoint for each attempt; logs name the attempt's endpoint |
| `SettingsWindow` | overlay, input | reading `SettingsKey` on each focus key press |

## Saving

- The current file is loaded with its comments, and each setting is written
  into it: SimpleIni keeps an existing key's comment and position, adds a new
  key with its description comment at the end of its section, and a missing
  section with its comment. Entries no setting reads are kept.
- A setting that is absent from the file and equal to its default stays absent,
  so a removed line keeps meaning "default". A cleared form whose default is set
  is written as an empty value.
- A missing or unreadable file is recreated from the commented default file.
- The text goes to `skyrim-mp.ini.tmp` and is renamed over the file, so a failed
  write never truncates it. A failed save is logged and reported in the window;
  the applied settings still last until restart.

## Verification

- Linux (done): the config round trip and headless window checks in Status.
- In game (pending): F10 opens the window with a cursor, and the game receives no
  input until it closes. Changing `SpawnsPerSecond` takes effect without a
  restart. Clearing a fallback removes it from remote players who used it, and
  setting one dresses them. A new endpoint disconnects, removes remote players,
  and connects to the new server; the status panel follows. A new
  `SettingsKey` opens the window and the old one no longer does. The file keeps
  hand-written comments after Apply. Chat still opens with Enter and gets no
  cursor.
