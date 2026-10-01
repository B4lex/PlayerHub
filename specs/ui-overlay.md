# UI Overlay

## Status

- **Overlay, input, status panel:** implemented and built on Windows; the
  settings UI runs in game. The renderer and input hook sites and the input
  context order were checked against the 1.7.104 executable and address
  library. SE 1.5.97 uses the same IDs and offsets as other public ImGui
  overlays but was not checked here. Skyrim VR is not supported: the hooks are
  not installed and a warning is logged. The nickname requirement's status
  panel line and the General tab's connection row are not built on Windows or
  run in game. On Linux, `StatusPanel.cpp` and `SettingsWindow.cpp` passed a
  syntax check against Dear ImGui 1.92.9, and `ConnectionControls.cpp` ran in
  a headless context of that version against stubbed `Network` functions:
  every state's button, status, and message, the message wrapping inside the
  window, and clicks reaching `SetConnectionEnabled` only while the button is
  enabled.
- **Chat:** implemented; the in-game check is pending. Channels: server unit
  tests cover routing and whisper addressing; on Linux, `ChatSync.cpp` and
  `WireCodec.cpp` ran headless against stubbed transport functions, decoding
  Go-encoded messages of each channel, rejecting malformed ones, and mapping
  typed prefixes, whisper labels, and rejection notices. `ChatWindow.cpp`
  colors were not run.
- **Settings window:** run in game. On Linux, `SettingsWindow.cpp` ran for
  several frames in a headless Dear ImGui 1.92.9 context with every tab drawn
  and simulated mouse and keyboard input: an invalid endpoint blocks Apply and a
  valid one is applied and saved. The mouse path of `InputCapture` was not run
  there. Closing with `SettingsKey` is not built on Windows or run in game. On
  Linux, `InputCapture.cpp` and `WindowManager.cpp` ran against stubbed engine
  input and ImGui IO functions: the key opens and closes the window without the
  game seeing either press, an editing key bound as `SettingsKey` stays with an
  active text field, and chat keeps Enter. `SettingsWindow.cpp` passed a syntax
  check.
- **Item picker and preview:** built on Windows; in game the picker lists names
  and the preview shows models. The attach pass for models that load in the
  background has not run in game yet. On Linux, `ItemCatalog.cpp`,
  `ItemPicker.cpp`, and the settings files ran headless with a fake catalog and
  a stand-in preview: per-key filters, multi-word and case-insensitive search,
  the preview following the hovered row, Enter picking the first match, Escape
  closing the list before the window, Apply saving `Plugin|0xID`, and
  `ItemPreview = false` hiding the preview. `ItemForms.cpp` and `ItemPreview.cpp`
  need CommonLibSSE-NG and were only checked in game.

## Scope

The plugin's own UI layer: a status panel, chat, the settings window, and the
reusable item picker with a 3D preview. `ui/` never changes the game; it reaches
the network only through the feature modules it presents, such as `ChatSync`,
and the configuration through `Config`.

## Technology

| Option | Result |
| --- | --- |
| Dear ImGui on Skyrim's D3D11 device | **Chosen.** One static library with no assets or runtime files. Immediate-mode code keeps a settings screen next to the values it edits, and many SKSE plugins draw this way, so it gets along with ENB, ReShade, and Community Shaders. |
| Scaleform (Flash) menus | Native look, but needs SWF tools, ActionScript, and a Papyrus/ESP or SkyUI MCM for settings: much tooling and cross-language glue for a few widgets. |
| Chromium Embedded Framework | Any web UI, but a runtime over 100 MB with separate processes, offscreen texture upload, and fragile Wine/Proton behavior. |

A Scaleform HUD can still be added beside the ImGui settings later without
touching the network side.

## Components

```text
src/ui/
  Overlay.h/.cpp              # renderer hook, ImGui context and backends, window registration
  Fonts.h/.cpp                # system fonts with fallbacks
  Window.h                    # interface every window implements
  WindowManager.h/.cpp        # registered windows, draw order, input focus
  InputCapture.h/.cpp         # input dispatch hook: focus keys, routing to ImGui
  Layout.h                    # placement helpers (screen corners)
  hud/StatusPanel             # connection, ping, traffic, plugin time
  chat/ChatWindow             # chat history and input
  settings/SettingsWindow     # plugin settings
  settings/ConnectionControls # connection button, status, and messages
  widgets/ItemPicker          # item dropdown
  preview/ItemPreview         # 3D item capture
src/sync/chat/ChatSync        # channel prefixes, send, rejections as notices, lines to display
src/runtime/PluginTime        # plugin time per pass
```

Every screen is an independent `UI::Window`. `Overlay::Install` registers each
with `WindowManager`, and `Overlay::DrawFrame` draws them in registration order.
Windows never reference each other, and a window's header exposes only a
factory, such as `CreateChatWindow`, so its state stays in its `.cpp`.

## Rendering

- `UI::Overlay::Install` runs at plugin load and uses 14 trampoline bytes. It
  replaces the `call` at `+0x9` in `BSGraphics::Renderer::End` (IDs
  75461/77246); on 1.7.104 that is `mov ecx, 1; call <timer>`, and the function
  later calls `IDXGISwapChain::Present` (`call [r9+0x40]` at `+0xA2`). The thunk
  calls the original first, then draws.
- The ImGui context is created on the first hooked frame with a device, context,
  window, and swap chain. A backend that fails to initialize turns the overlay
  off for the session with an error.
- `io.DisplaySize` comes from the swap chain's back buffer, not the window's
  client rectangle. Sizes and fonts are written for 1080p and scaled by
  `backBufferHeight / 1080` (at least 0.5).
- The DX11 backend does not save the output merger, so the overlay binds the
  swap chain's render target view, draws, and restores the previous targets.
- `ImGuiConfigFlags_NoMouse` and `NoMouseCursorChange` are set unless a window
  takes the mouse, and `imgui.ini` is disabled.

### Fonts

`Fonts::Load` reads from the system fonts folder (`FOLDERID_Fonts`, which is
`C:\windows\Fonts` in a Wine or Proton prefix). The first file found is the main
font, and each later one is merged and only supplies glyphs the earlier lack:

| Files | Adds |
| --- | --- |
| `segoeui.ttf`, `arial.ttf` | Latin, Greek, Cyrillic |
| `tahoma.ttf`, `micross.ttf` | Arabic, Hebrew, Thai |
| `Nirmala.ttc`, `nirmala.ttf` | Indic scripts |
| `msyh.ttc`, `msyh.ttf` | Chinese, and kana and Hangul in Proton's copy |
| `YuGothM.ttc`, `msgothic.ttc` | Japanese |
| `malgun.ttf` | Korean |
| `seguisym.ttf` | Symbols |

Windows has Segoe UI; Proton prefixes have Arial and the rest except Segoe UI,
Yu Gothic, and Segoe UI Symbol. Missing files are skipped. Each file is
memory-mapped read-only and given to the atlas with
`FontDataOwnedByAtlas = false`, which since 1.92 means the atlas reads it in place for its lifetime, so
the views are never unmapped, and only pages with drawn glyphs are read: CJK
fonts cost address space, not memory. Dear ImGui 1.92 rasterizes glyphs on
demand, so no glyph ranges are needed. Without any file, ImGui's default font
covers Latin only. The log lists the fonts loaded. Han characters take the
Chinese forms of Microsoft YaHei, since it comes before the Japanese fonts.

## Input focus

At most one window holds input focus. While one does, ImGui receives the
keyboard and the game receives no keyboard or mouse input.

- A window names its focus keys with `Window::IsFocusKey` (DirectInput scan
  codes). `InputCapture` gives it focus when such a key is pressed during
  gameplay with no window focused, and the game never sees that press. The key
  is inert until the overlay has drawn once.
- Gameplay means the game is not paused and `kGameplay` is the active input
  context, the last entry of `ControlMap::contextPriorityStack`: 1.7.104's
  `PushInputContext` appends, keeping the console on top. Any menu, the console,
  dialogue, or a loading screen therefore blocks focus keys, and a context taking
  over input releases a focused window.
- A window gives focus back with `WindowManager::ReleaseFocus(*this)`, or names
  release keys with `Window::IsReleaseKey`: `InputCapture` releases the focused
  window when such a key is pressed, and neither the window nor the game sees
  that press. A release key that ImGui also receives, such as Delete or Home,
  goes to a text field instead while one is active (`io.WantTextInput` as of
  the overlay's latest frame), so it still edits.
- `InputCapture` replaces the `call` at `+0x7B` in
  `BSInputDeviceManager::PollInputDevices` (IDs 67315/68617), which is
  `BSTEventSource<InputEvent*>::SendEvent(this, &head)` on 1.7.104 and feeds the
  menu and player-control sinks. The engine calls `ClearInputQueue` right after,
  and `PushOntoInputQueue` resets each event's `next`, so the thunk relinks the
  list to drop the events the game must not see. It refuses to patch anything
  but a `call`.
- Every event of a key or button goes where its press went: a key held while a
  window takes focus is still released to the game, and a key that closed a
  window is not.
- Key presses and releases become ImGui key events: editing and navigation keys,
  modifiers, and the letters of editing shortcuts (scan codes are physical
  positions). Typed text comes from Skyrim's `CharEvent`s. Tab is not forwarded,
  so it cannot move focus off a text field. Gamepad input always reaches the
  game.
- A window that returns true from `Window::UsesMouse` gets mouse movement and
  buttons through ImGui: `InputCapture` clears `NoMouse`, sets
  `io.MouseDrawCursor`, and moves an absolute cursor by DirectInput's relative
  counts, scaled by `backBufferHeight / 1080` and clamped to the back buffer. The
  cursor starts centered whenever such a window takes focus, and its position is
  queued after the Win32 backend's, which follows the hidden system cursor. Left,
  right, and middle buttons and the wheel go to ImGui. A keyboard-only window
  gets the wheel as `WindowInput::wheelSteps`.
- Events are queued under a lock and applied in `Overlay::DrawFrame` before
  `ImGui::NewFrame`, since the renderer hook is not guaranteed to run on the
  main-loop thread. Losing focus sends `AddFocusEvent(false)`, which releases
  every key ImGui holds.
- Characters outside the Basic Multilingual Plane, such as emoji, never reach a
  window: Skyrim keeps only single-unit `ToUnicode` results when building
  `CharEvent`s (checked against 1.7.104).

## Status panel

The panel sits in the top-right corner, 12 px from both edges before scaling, on
a translucent background, and takes no input. It is drawn only during a game
(`GameSession::IsInGame`) and while `[UI] StatusPanel` is on; a change applies
on the next frame.

| State | Text | Color |
| --- | --- | --- |
| No nickname set | `Set a nickname (F10)`, with the current `SettingsKey` | red |
| Not connected, whatever the reason | `Disconnected` | red |
| Connected, no sample yet or timed out | `Ping -- ms` | grey |
| ≤ 80 ms / ≤ 150 ms / > 150 ms | `Ping N ms` | green / yellow / red |

While connected, two grey lines follow with the received and sent traffic in
KB/s ([Network](network.md#traffic)), `--` before the first window. Always
shown below: `Plugin N.NN ms`, or `Plugin -- ms` before the first window.

The panel does not say why a connection is down or a rename failed: a refusal
needs a sentence and an action in the settings window, so those messages are
on its General tab ([Connection row](#connection-row)). The nickname line stays
because it names the key that opens the window.

### Plugin time

`PluginTime::Scope` wraps the plugin's work in the main-loop hook and the
renderer hook, adding its `std::chrono::steady_clock` duration to a shared
atomic total, since the renderer hook may run on another thread. The main-loop
hook calls `PluginTime::EndFrame` once per pass after its scope closes; time
before the first pass is dropped. Once at least 1 s of wall-clock time has
passed, the total over the passes in the window becomes the published average
and a new window starts, so the number changes once per second. It is always
compiled in and costs two clock reads per hook call. Detailed profiling uses
Tracy (`xmake f --tracy=y`, `runtime/Profiling.h`).

## Chat

Players talk in plain text on three channels. Chat is presentation only. The
wire, routing, and server rules are in [Network](network.md#chat).

Goal: talk to everyone, to players nearby, or to one player, and tell the three
apart at a glance. Non-goals: chat history for late joiners, switching
channels other than by prefix, replying to the last whisper.

| Typed | Channel | Reaches |
| --- | --- | --- |
| `!text` | global | every connected player |
| `text` | local | placed players within `chat.local_radius` (8192) in the same worldspace, or in the same interior |
| `@Name text` | whisper | one player, by nickname |

- `ChatSync::SendLocalMessage` trims the line, takes the channel from its first
  character, drops the prefix, and trims again. An empty line sends nothing,
  `@` alone adds a notice. It validates the text (`protocol/ChatMessage.h`,
  shared with the parser), and `Network::PublishChatMessage` sends `ChatSend`
  with the channel and a request ID; a rejecting `Result` runs its callback on
  the transport thread. Being disconnected and each server rejection
  (`rate limited`, `not in world`, `unknown recipient`, `empty whisper`) become
  a local notice line.
- A whisper's sender reads `To <recipient>` for the local player's own whisper
  and `From <sender>` otherwise; other channels show the sender's label.
  `ChatSync::DrainLines` returns received messages and notices. Neither needs
  the game thread.
- `ChatWindow` keeps the latest 100 lines, each stamped with `ImGui::GetTime()`
  when it arrived. Senders are shown as a player label
  ([Appearance and equipment](appearance-and-equipment.md#player-label)).
- Enter or keypad Enter during gameplay opens the chat and focuses its input.
  Typing goes only to the chat; the character does not move or look around, and
  keys already held keep acting until released.
- Enter sends and closes; Enter on an empty line only closes. Escape discards the
  text typed since opening and closes. Every key, console and menu keys
  included, goes to the chat while it is open, but a menu that opens by itself,
  such as dialogue an NPC starts or a loading screen, closes it and keeps the
  draft.
- Open, it sits in the lower-left corner above the magicka bar, 30 font sizes
  wide with 10 lines of history. The wheel scrolls 3 lines per notch and Page
  Up/Down a page. New lines scroll down unless the history was scrolled back.
- Closed, the newest lines (at most 6) show for 10 s on a faint background and
  fade out over the last 2; with nothing recent nothing is drawn.
- Sender names take the channel's color: gold for global, lime for local, and
  violet for whispers. Text is light grey and notices orange. The chat has no
  mouse cursor.

## Settings window

Players change `skyrim-mp.ini` ([Configuration](configuration.md)) in game and
see the result at once. The game is not paused; the world keeps running and
peers keep seeing the player.

- `[UI] SettingsKey` opens it during gameplay as a focus key and closes it again
  as a release key ([Input focus](#input-focus)); a changed key applies once
  applied. The title bar's close button or Escape also closes it; Escape first
  goes to an active text field or open combo or list. Any menu taking input
  closes it too.
- Every opening starts from the live configuration; closing discards unapplied
  edits.
- Centered, 44 × 32 font sizes, one tab per file section in schema order:
  General, which opens first and holds `Nickname` and the
  [connection row](#connection-row), then Network, Default Outfit, Performance,
  and UI. Only a tab's content scrolls; the buttons stay below it. It uses the
  mouse.
- Widgets come from the same `Config::Describe` visitor that reads and writes the
  file, so a new setting appears without UI code: a checkbox for a bool, a
  clamped number field for an unsigned value, a text field for a string, an item
  picker for a form, and a combo for a choice or key. Section and group comments
  are shown as notes.
- Text that does not parse turns red and leaves the setting at its last valid
  value; Apply stays disabled while any field is red. Apply is enabled when the
  edits differ from the live settings. Revert returns to the live settings and
  Defaults to the built-in defaults, without applying. The footer reports
  unapplied changes, a successful save, or a failed one.

### Connection row

The General tab ends with a `Connection` row below a separator, after the
section's settings, drawn by `ConnectionControls`: a button, an indicator
circle, green while connected and red otherwise, and the status as text. The
button follows `Network::GetConnectionState`, so its label never claims a state
the transport has not reached, and keeps its width and identity when the label
changes, so the status does not move:

| State | Button | Press | Status |
| --- | --- | --- | --- |
| Connected | `Disconnect` | stops wanting a connection | `Connected, ping N ms`, or `Connected` before a sample |
| Connecting | `Cancel` | stops wanting a connection | `Connecting...` |
| Connecting, refused by the server | `Cancel` | stops wanting a connection | `Not connected` |
| Disconnecting | `Disconnecting...`, disabled | — | `Disconnecting...` |
| Disconnected | `Connect` | wants a connection again | `Disconnected` |

A message follows on its own wrapped lines below the row while one applies.
This is the only place the plugin shows them:

| Condition | Message | Color |
| --- | --- | --- |
| The server refused the connection | the sentence of `Network::GetConnectionRefusal` | red |
| Connected, and the server refused the applied nickname | the sentence of `Network::GetNicknameRejection`; peers still see the previous nickname | red |
| Disconnected with no applied nickname | `Enter a nickname and apply it to connect.` | grey |

Without an applied nickname `Connect` is disabled. If the player's choice
already wants a connection, as after `AutoConnect`, applying a nickname
connects without a press. The same holds after the server refused a nickname:
applying another one connects. After another refusal, `Cancel` then `Connect`
retries. The refusals and the rename are in [Network](network.md#connection).

Not wanting a connection closes it and removes every remote player until
Connect. The button acts on the live settings; an edited nickname or endpoint
must be applied first, and applying an endpoint while connected moves the
connection to it ([Network](network.md#connection)).

## Item picker

`UI::ItemPicker::Draw(id, value, filter)` draws one form field as a searchable
dropdown of loaded items that fit the setting's filter, instead of typing
`Plugin|FormID`. A window keeps one picker for all its fields: it caches each
field's label until the value changes, and only one list can be open. The
catalog and filters are in
[Appearance and equipment](appearance-and-equipment.md#item-filter-and-catalog).

- The closed field shows the item's name, `Plugin|0xID (not loaded)` in amber
  for a missing plugin, or the name in amber for an item the filter rejects,
  each with an explaining tooltip. A value for a plugin that is not loaded can
  still be written to the file.
- The open list is 40 × 22 font sizes, with a search field that takes keyboard
  focus and a two-column table of name and plugin. There is no `None` row: a
  form setting always has an item. Only visible rows are drawn
  (`ImGuiListClipper`), and the current value is scrolled into view.
- Clicking a row picks it, Enter picks the first match, Escape closes the list.
- `Reset` beside the field sets the setting's default; it is disabled at the
  default, and its tooltip names the default item.
- With `[UI] ItemPreview`, a 13-font-size square panel to the right shows the
  hovered item's model, or the current value's, with its name and
  `Plugin|0xID` below. Race lists have no panel, since the inventory renderer
  shows only items (`Forms::HasModel`).

### 3D preview

`UI::ItemPreview` shows one item at a time through the game's own inventory
renderer, following `Item3DPreview` in ModExplorerMenu. Models cannot be rotated
or zoomed.

- `Draw(formID, size)` records the request and draws the latest capture, fitted
  and centered on a dark background. A new item shows nothing for 0.3 s, then
  "Loading...".
- `Update` runs in `Overlay::DrawFrame` before `ImGui::NewFrame`, when the game
  frame is finished and the overlay has not drawn. If the previous frame drew a
  preview, it calls `Inventory3DManager::Begin3D(kInventory)` once and
  `LoadInventoryItem` whenever the form changes. A model loading in the
  background is attached only by `Render`, so while none is attached, `Update`
  calls `Render` with `UI::numDisablePauseMenu` at zero, which draws nothing.
  Once a model is attached, it projects the model's world bound through
  `UI3DSceneManager::viewFrustum` to a screen rectangle, then saves that
  rectangle of the back buffer, clears it to the preview background
  (`ID3D11DeviceContext1::ClearView`), calls `Render` with the count at least one,
  copies the rectangle into a 2048² texture ImGui samples, and puts the saved
  pixels back.
- The capture is opaque but the back buffer's alpha under the model is not
  coverage, so the image is drawn with blending off (a draw callback sets an
  opaque blend state, then `ImDrawCallback_ResetRenderState`).
- When no window drew a preview in the previous frame, `Update` unloads the
  item, calls `End3D`, and frees the textures. If an item menu opened meanwhile
  (`UI::IsItemMenuOpen`), that menu owns the renderer, so the state is dropped
  without calling it.
- The preview turns itself off for the session with one warning when the overlay
  does not run on the bound main-loop thread (`GameThread::IsAuthoritative`), the
  inventory renderer is missing, or capture textures cannot be created for the
  back buffer format.
- Unlike ModExplorerMenu, the game is not paused. In 1.7.104 `Render` attaches a
  finished load (load task state 5), updates the model, and tail-calls the UI 3D
  scene render (ID 52727), which draws nothing unless the scene's light scheme
  matches and `UI::numDisablePauseMenu` (`UI + 0x168`) is positive. The count is
  positive while an inventory menu is open, and while ModExplorerMenu's menu is,
  because it sets `kDisablePauseMenu` only when it pauses. The overlay is not a menu, and opening one would take input
  focus, so `ItemPreview` sets the count for the call and restores it. The scene
  draws into the engine's current render target, which a one-time pixel readback
  confirmed is the back buffer at the overlay hook; with the count at zero every
  captured pixel matched the cleared background.
- Logs: `Item preview started` and `stopped` (debug), the first capture of each
  start with its screen box and bound radius, `Item preview waiting` once per
  item whose model has not appeared a second after loading started (with the
  models `Inventory3DManager` holds, its load task state, and both light
  schemes), and `Item preview unavailable: <reason>` once.

## Acceptance criteria

1. The main menu shows no status panel. In a game the panel shows the connection,
   a stable ping, traffic, and plugin time, and scales at 1080p and 4K;
   `StatusPanel = false` hides it at once.
2. Enter opens the chat only with no menu open; the character stands still while
   typing, and W held from before keeps running until released. Enter sends and
   the line appears for both players; Escape closes without sending and does not
   open the journal; a menu that opens by itself closes the chat; recent lines
   fade after 10 s; the wheel and Page Up/Down scroll; sending while disconnected
   shows a notice.
3. `!hi` reaches every player with a gold name; `hi` reaches players within
   8192 units or in the same interior with a lime name, and not a player
   farther away; `@Name hi` reaches only that player, reading `To Name` for the
   sender and `From Sender` for the recipient in violet; an unknown name shows
   a notice.
4. `F10` opens the settings window with a cursor, and the game receives no input
   until it closes; `F10` again closes it. A new `SettingsKey` opens and closes
   it and the old one no longer does.
   Chat still opens with Enter and gets no cursor.
5. The General tab's connection row reads `Connected, ping N ms` with a green
   indicator once connected, and `Disconnected` with a red one on Disconnect,
   which removes remote players and keeps them away through a save load until
   Connect. Connect right after Disconnect shows `Cancel` until the connection
   opens, never `Disconnect` while disconnected. A nickname another player
   holds shows the refusal's sentence below the row and only `Disconnected` in
   the status panel; applying another nickname connects and clears it.
6. The picker lists only matching base-game items and races. Hovering rows
   switches the model without flicker on the world behind; background loads
   appear after the attach pass (a stall logs `Item preview waiting`); nothing is
   left on screen after the list closes; the inventory shows its own models
   normally afterwards; FPS stays steady while a list is open;
   `ItemPreview = false` removes the panel.

## Known limitations

- A script none of the system fonts cover shows placeholders; emoji cannot be
  typed.
- Editing shortcut letters (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY positions,
  not the keyboard layout. Chat text cannot be selected with the mouse.
- A whisper cannot address `Ulf` with a message starting with `Stone` while a
  player named `Ulf Stone` is online, since the longest name wins.
- The picker covers armor, weapons, torches, ammunition, and races, not spells
  or actors; `ItemKind` is where more kinds would go.
