# UI Overlay and Ping Indicator

## Status

Implemented, pending a Windows build and in-game check. The renderer hook site
was checked against the 1.7.104 executable and address library. SE 1.5.97 uses
the same ID and offset as other public ImGui overlays but has not been checked
here. Skyrim VR is not supported: the hook is not installed and a warning is
logged.

## Goal

Give the plugin its own UI layer. The first use is a small indicator in a screen
corner showing whether the client is connected, its round-trip latency to the
server, and how much frame time the plugin itself takes. Later, the same layer has to host settings that players can change.

## Technology choice

| Option | Result |
| --- | --- |
| Dear ImGui on Skyrim's D3D11 device | **Chosen.** One static library, with no assets or extra runtime files. Immediate-mode code keeps a settings screen close to the values it edits, and many SKSE plugins draw this way, so it gets along with ENB, ReShade, and Community Shaders. |
| Scaleform (Flash) menus | Looks native, but needs SWF authoring tools, ActionScript, and a Papyrus/ESP or SkyUI MCM for settings. That adds a lot of tooling and cross-language glue for a few widgets. |
| Chromium Embedded Framework | Can render any web UI, but it is a runtime of more than 100 MB with separate processes, offscreen texture upload, and fragile behavior under Wine/Proton. That is too much for a HUD and a settings panel. |

If a native look matters later, a Scaleform HUD can still be added beside the
ImGui settings screen without changing the network side.

## Source layout

```text
src/ui/
  Overlay.h/.cpp            # renderer hook, ImGui context/backends, window registration
  Fonts.h/.cpp              # system fonts with fallbacks for non-Latin scripts
  Window.h                  # interface every overlay window implements
  WindowManager.h/.cpp      # registered windows, draw order, input focus
  InputCapture.h/.cpp       # input dispatch hook: focus keys, routing to ImGui
  Layout.h                  # shared placement helpers (screen corners)
  hud/StatusPanel.h/.cpp    # always-on connection, ping, and plugin time widget
  chat/ChatWindow.h/.cpp    # global chat, see specs/020-player-chat.md
  settings/SettingsWindow.h/.cpp  # plugin settings, see specs/021-in-game-settings.md
  settings/ConnectionControls.h/.cpp  # connection indicator and Connect/Disconnect button
  widgets/ItemPicker.h/.cpp  # reusable item dropdown, see specs/022-item-picker-and-preview.md
  preview/ItemPreview.h/.cpp # 3D item model capture for windows
src/network/
  ConnectionMonitor.h/.cpp  # when to connect, probe cadence, timeout, latest sample, reconnect schedule
src/runtime/
  PluginTime.h/.cpp         # plugin time per main-loop pass, averaged each second
  GameSession.h/.cpp        # whether a game is loaded, set by PresenceSync
```

`ui/` never changes the game; it reaches the network only through the feature
modules it presents, such as `ChatSync`. Every screen is an independent
`UI::Window`: `Overlay::Install` registers each one with `WindowManager` at
startup, and `Overlay::DrawFrame` draws them all in registration order. Windows
never reference each other. A window's header exposes only a factory, such as
`CreateChatWindow`, so its state stays in its own `.cpp`. Add later screens as
siblings and register them in `Overlay::Install`.

## Input focus

At most one window holds input focus. While one does, Dear ImGui receives the
keyboard and the game receives no keyboard or mouse input.

- A window names its focus keys with `Window::IsFocusKey` (DirectInput scan
  codes). `InputCapture` gives it focus when such a key is pressed during
  gameplay with no window focused, and the game never sees that press. The key
  is inert until the overlay has drawn once.
- Gameplay means the game is not paused and `kGameplay` is the active input
  context, the last entry of `ControlMap::contextPriorityStack`: 1.7.104's
  `PushInputContext` appends, keeping the console on top. Any menu, the console,
  dialogue, or a loading screen therefore blocks focus keys, and taking over
  input while a window is focused releases its focus.
- A window gives focus back with `WindowManager::ReleaseFocus(*this)`.
- `InputCapture` replaces the `call` at `+0x7B` in
  `BSInputDeviceManager::PollInputDevices` (IDs 67315/68617), which is
  `BSTEventSource<InputEvent*>::SendEvent(this, &head)` on 1.7.104 and feeds the
  menu and player-control sinks. The engine calls `ClearInputQueue` right after,
  and `PushOntoInputQueue` resets each event's `next`, so the thunk relinks the
  list to drop the events the game must not see. SE uses the same offset as
  other public ImGui overlays but has not been checked here; the detour refuses
  to patch anything but a `call`.
- Every event of a key or button press goes where its press went. A key held
  while a window takes focus is still released to the game, and a key that
  closed a window is not released to it.
- Keyboard presses and releases become ImGui key events: editing and navigation
  keys, modifiers, and the letters used by editing shortcuts (scan codes are
  physical positions). Typed text comes from Skyrim's `CharEvent`s. Tab is not
  forwarded, so it cannot move keyboard focus off a text field. Gamepad input
  always reaches the game.
- Mouse movement and buttons never reach the game while a window has focus. A
  window that returns true from `Window::UsesMouse` gets them through Dear
  ImGui: `InputCapture` clears `ImGuiConfigFlags_NoMouse`, sets
  `io.MouseDrawCursor`, and moves an absolute cursor by DirectInput's relative
  counts, scaled by `backBufferHeight / 1080` and clamped to the back buffer.
  The cursor starts in the middle of the screen whenever such a window takes
  focus, and its position is queued after the Win32 backend's, which follows
  the hidden system cursor. Left, right, and middle buttons and the wheel go
  to ImGui. A keyboard-only window gets no mouse and receives the wheel as
  `WindowInput::wheelSteps` instead.
- Events are queued under a lock and applied in `Overlay::DrawFrame` before
  `ImGui::NewFrame`, since the renderer hook is not guaranteed to run on the
  main-loop thread. Losing focus sends `AddFocusEvent(false)`, which releases
  every key ImGui holds.

## Rendering

- `UI::Overlay::Install` runs at plugin load and uses 14 trampoline bytes. It
  replaces the `call` at `+0x9` in `BSGraphics::Renderer::End` (IDs 75461/77246).
  On 1.7.104 that is `mov ecx, 1; call <timer>`, and the same function later
  calls `IDXGISwapChain::Present` (`call [r9+0x40]` at `+0xA2`). The thunk
  calls the original function first, then draws.
- The ImGui context is created on the first hooked frame that has a device,
  context, window, and swap chain. If a backend fails to initialize, the
  overlay turns itself off for the session and logs an error.
- `io.DisplaySize` comes from the swap chain's back buffer, not the window's
  client rectangle. Sizes and fonts are written for 1080p and scaled by
  `backBufferHeight / 1080` (at least 0.5). Dear ImGui 1.92 rasterizes glyphs
  on demand at the size they are drawn, so the fonts below need no glyph ranges.

## Fonts

`Fonts::Load` reads from the system fonts folder (`FOLDERID_Fonts`, which is
`C:\windows\Fonts` in a Wine or Proton prefix) the files in this order: the
first one found is the main font, and each later one is merged and only
supplies glyphs the earlier ones lack.

| Files | Adds |
| --- | --- |
| `segoeui.ttf`, `arial.ttf` | Latin, Greek, Cyrillic |
| `tahoma.ttf`, `micross.ttf` | Arabic, Hebrew, Thai |
| `Nirmala.ttc`, `nirmala.ttf` | Indic scripts |
| `msyh.ttc`, `msyh.ttf` | Chinese, also kana and Hangul in Proton's copy |
| `YuGothM.ttc`, `msgothic.ttc` | Japanese |
| `malgun.ttf` | Korean |
| `seguisym.ttf` | Symbols |

Windows has Segoe UI; Proton prefixes have Arial and the rest except
Segoe UI, Yu Gothic, and Segoe UI Symbol. Missing files are skipped. Each file
is memory-mapped read-only and handed to the atlas with
`FontDataOwnedByAtlas = false`, which since 1.92 means the atlas reads it in
place for its whole lifetime, so the views are never unmapped. Only the pages
holding drawn glyphs are read, so the CJK fonts cost address space rather than
memory. If no file loads, the overlay uses Dear ImGui's default vector font,
which covers Latin only. The log lists the fonts that loaded.

Han characters take the Chinese forms of Microsoft YaHei, since it comes before
the Japanese fonts. Characters outside the Basic Multilingual Plane, such as
emoji, never reach a window: Skyrim keeps only single-unit `ToUnicode` results
when it builds `CharEvent`s (checked against 1.7.104).
- The DX11 backend does not save or restore the output merger. The overlay binds
  the swap chain's render target view, draws, and then restores the previous
  targets.
- `ImGuiConfigFlags_NoMouse` and `NoMouseCursorChange` are set, and `imgui.ini`
  is disabled. The keyboard reaches ImGui only through `InputCapture`.

## Latency measurement

- The client emits `latency:probe` with no arguments and an ack callback. The
  server returns `{"ok": true}` right away and does not relay or store anything.
- The main-loop hook calls `Network::UpdateConnectionMonitor` on every pass,
  after `SyncCoordinator::OnFrame`. Frame time stops in the main menu and while
  the game is paused; the monitor then gets a zero delta, so its timers hold
  while opening and closing still follow the game and the player's choice. The
  client sends a probe every 1 second after the previous one was sent. Only
  one probe is outstanding at a time. The interval
  and timeouts follow the frame-delta rules in `AGENTS.md`.
- The round trip itself is measured with `std::chrono::steady_clock` from emit to
  ack, so frame rate does not affect the number.
- Each probe has an ID, and a compare-exchange on the outstanding ID accepts only
  the current probe's ack. Acks from abandoned probes or older connections are
  ignored. A disconnect or a new connection generation clears the sample.

## When the client connects

A connection is wanted only in a game and while the player's choice enables
it. `ConnectionMonitor` owns that decision; nothing else opens or closes the
client.

- At `kDataLoaded`, `Network::Initialize` only registers the client's listeners.
  The main menu never connects.
- A game starts when `PresenceSync` first finds the player placed in the world,
  and ends when the main menu opens (`GameSession::SetInGame`). Loading
  screens, including loading another save from in game, keep the game and its
  connection; `player:exit` and `player:enter` still mark leaving and entering
  the world.
- When the monitor sees a game start, the player's choice becomes
  `[Network] AutoConnect` (default `true`). With it off, the game stays
  disconnected until Connect.
- The settings window's buttons (spec 021) change the choice with
  `Network::SetConnectionEnabled`. It lasts until the game ends, so a save
  loaded from in game keeps it and the next game from the main menu starts
  from `AutoConnect` again.
- Opening skips the reconnect delay and resets the attempt count, then follows
  the usual attempts below.
- Closing: `BeginClose` marks the client disconnected at once, forgetting every
  remote player, and runs `sync_close` on a worker. It shares the worker slot
  with `BeginReconnect`, so while an attempt's worker is busy the monitor stays
  in its closing phase and retries each pass. A namespace that still opens
  after the close was requested is ignored until the next `BeginReconnect`.
- socket.io-client-cpp 3.1.0 closes a connected namespace by sending
  `DISCONNECT` and waiting 3 seconds for a reply that a v4 server never sends
  (`socket::impl::close`). The transport closes within milliseconds, but the
  worker's `sync_close` returns only after that timer, so a connection requested
  within 3 seconds of a close starts when it expires.
- `Network::GetConnectionState` combines whether a connection is wanted with
  the transport, never running ahead of it: `kConnected` while the namespace is
  open, `kConnecting` while one is wanted but not open (including the wait
  above), `kDisconnecting` while none is wanted but the transport is still open,
  and `kDisconnected` otherwise.

## Connection loss and recovery

The client is disconnected exactly when the default namespace is not open, and
the overlay shows that state whichever side ended it. Every loss while a
connection is wanted is recoverable.

- Detection, server side: a server restart, an Engine.IO ping timeout, or a
  namespace `DISCONNECT` reaches the client as a namespace close, a transport
  close, or a failed connect. All three mark the client disconnected and forget
  every remote player.
- Detection, silent server: the library does not notice a server that stops
  answering. A probe with no ack after 5 seconds of accumulated, clamped frame
  time makes `ConnectionMonitor` call `AbandonConnection`, which marks the client
  disconnected exactly as a reported close would.
- The library's own retry is disabled (`set_reconnect_attempts(0)`). It skips any
  close with a normal status, which is how a server drops a client, and it leaves
  the namespace closed on a live transport. Instead each loss is reported once
  and `ConnectionMonitor` reopens the connection.
- Recovery: after a loss the monitor waits 1 second, then calls
  `BeginReconnect`, doubling the wait after each failed attempt up to 15 seconds.
  Each wait is scaled by a random factor in 0.5–1.5, so clients that lost the
  same server do not all retry together.
  Attempts are unlimited. An attempt that is reported failed, or that has not
  opened the namespace after 20 seconds, is followed by the next one.
- `BeginReconnect` runs on a worker thread: `sync_close` joins the old transport
  thread, so nothing from the dead connection is reported afterwards. It then
  registers the event handlers again, because the library drops the socket object
  and its handlers when the server closes the namespace, and calls `connect`.
- A reopened namespace advances the connection generation, so latest-wins
  publishers republish their state. Remote players are recreated from the
  server's replay.

## Status panel

The panel sits in the top-right corner, 12 px from both edges before scaling,
on a translucent background. It takes no input. It is drawn only during a game
(`GameSession::IsInGame`), so never in the main menu, and only while
`[UI] StatusPanel` is on (default `true`). A changed setting applies on the next
frame.

| State | Text | Color |
| --- | --- | --- |
| Not connected | `Disconnected` | red |
| Connected, no sample yet or timed out | `Ping -- ms` | grey |
| ≤ 80 ms | `Ping N ms` | green |
| ≤ 150 ms | `Ping N ms` | yellow |
| > 150 ms | `Ping N ms` | red |

Below the ping line, always shown whatever the connection state:

| State | Text | Color |
| --- | --- | --- |
| First window not complete | `Plugin -- ms` | grey |
| Otherwise | `Plugin N.NN ms` | grey |

## Plugin time

- `PluginTime::Scope` wraps the plugin's work in the main-loop hook and in the
  renderer hook. Scopes add their duration, measured with
  `std::chrono::steady_clock`, to a shared atomic total, because the renderer
  hook is not guaranteed to run on the main-loop thread.
- The main-loop hook calls `PluginTime::EndFrame` once per pass, after its
  scope closes. Time spent before the first pass, such as startup, is dropped.
- Once at least 1 second of wall-clock time has passed since the window started,
  the total divided by the passes in the window becomes the published average,
  and a new window starts. The overlay reads that average, so it changes once
  per second instead of flickering every frame.
- It is always compiled in and costs two clock reads per hook call, so it needs
  no build option.

## Verification

- Server: `test_app_latency.py` checks that the probe is acknowledged with
  `{"ok": true}` and not relayed, and that a client-supplied `sentAtMs` is
  neither stored nor relayed.
- In game (pending): the main menu stays disconnected, shows no status panel,
  and the log shows no connection attempt. With `AutoConnect = false`, loading
  a save stays disconnected until Connect. `StatusPanel = false` hides the panel
  at once. Loading a save connects once the player is in the world,
  and the panel then shows a stable ping. A door's loading screen keeps the
  connection; quitting to the main menu closes it after `player:exit`.
  Stopping the server shows `Disconnected`, and a restart
  recovers. So do a server-side kick, and a blocked route to the server, which
  is noticed after the 5-second probe timeout. The overlay scales at 1080p and
  4K, and nothing new appears in the log apart from the state-change lines.
