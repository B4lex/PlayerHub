# UI Overlay

## Status

- **Overlay, input, status panel:** implemented and built on Windows; the
  settings UI runs in game. The renderer and input hook sites and the input
  context order were checked against the 1.7.104 executable and address library.
  SE 1.5.97 uses the same IDs and offsets as other public ImGui overlays but was
  not checked here. Skyrim VR is not supported: the hooks are not installed and
  a warning is logged. The status panel reading only `Disconnected` without a
  display name is not built on Windows or run in game; on Linux,
  `StatusPanel.cpp` passed a syntax check against Dear ImGui 1.92.9.
- **Chat:** implemented; the in-game check is pending. Channels: server unit
  tests cover routing and whisper addressing; on Linux, `ChatSync.cpp` and
  `WireCodec.cpp` ran headless against stubbed transport functions, decoding
  Go-encoded messages of each channel, rejecting malformed ones, and mapping
  typed prefixes, whisper labels, and rejection notices. `ChatWindow.cpp`
  colors were not run.
- **Settings window, theme, connection controls:** an earlier layout, with tabs
  and a title bar, ran in game. The sidebar layout, the display name's line
  with the connection button, the switches, and the theme are not built on
  Windows or run in game. On Linux, `SettingsWindow.cpp`,
  `ConnectionControls.cpp`, `Controls.cpp`, `ItemPicker.cpp`, and `Theme.cpp`
  ran in a headless Dear ImGui 1.92.9 context against stubbed `Config::Apply`,
  `Network`, and `ItemPreview` functions and a fake catalog, with simulated
  mouse and keyboard input and the draw data rasterized to images over a
  stand-in scene. With General as a row and a button below it:
  - every section at 1080p, General at 720p and Default Outfit at 4K, the open
    key, sex, and item lists, a label's tooltip, an invalid field with its
    footer, and a picked item with its reset button;
  - typing applies nothing; Enter and a click on another section each apply
    the text once; Escape puts the previous text back and leaves the window
    open; invalid text is not applied; a switch and a step button apply at
    once; an outfit pick applies only on Apply, and stays pending through an
    edit in another section;
  - the close button and Escape close the window; a drag on the sidebar or
    beside the title moves it and one on the lower right corner resizes it.

  General ran the same way at 1080p only, with the real schema and a stand-in
  item picker. Its layout ran against stubbed `Network` functions, before the
  field was locked and `Connect` became the only way to connect:
  - the field and the button share a line in every state; the field has the
    keyboard when the window opens without a display name and not with one;
  - invalid or emptied text disables `Connect`, and a click does not connect
    with it; Escape puts the name back; `Disconnect` and `Cancel` stop wanting
    a connection, and a press while disconnecting does nothing.

  General as it is now ran with the real `ConnectionMonitor` over a stubbed
  transport and game state:
  - typing applies nothing and enables `Connect`; Enter and a click elsewhere
    each apply the name and do not connect, also in a game started with
    `AutoConnect` and no display name; one click on `Connect` applies a typed
    name and connects, once;
  - while connecting and while connected the field is disabled, a click,
    typing, and Enter change nothing, and hovering it shows its tooltip;
    `Disconnect` unlocks it, and a name applied then does not connect;
  - a refusal for the display name, for the protocol version, and of a
    replaced session each leave `Connect`, an editable field, and the sentence
    under it; applying another name keeps the sentence and does not connect;
    `Connect` clears it and connects.

  ImGui's style-stack checks passed. The mouse path of `InputCapture` was not
  run there. Closing with `SettingsKey` is not built on Windows or run in game;
  on Linux, `InputCapture.cpp` and `WindowManager.cpp` ran against stubbed
  engine input and ImGui IO functions: the key opens and closes the window
  without the game seeing either press, an editing key bound as `SettingsKey`
  stays with an active text field, and chat keeps Enter.
- **Themed faces (Sovngarde):** not built on Windows or run in game. On Linux,
  `Fonts.cpp` ran with POSIX stand-ins for its Win32 file calls, with
  `SettingsWindow.cpp`, `ConnectionControls.cpp`, `Controls.cpp`, and
  `Theme.cpp`, in a headless Dear ImGui 1.92.9 context against a folder laid
  out as the deploy step leaves it and stand-in system fonts, the draw data
  rasterized to images:
  - every section at 1080p and Default Outfit at 4K, the key list, and a
    label's tooltip are in Sovngarde with bold headings; a display name with
    `Ґ`, `ž`, and Han characters shows them from the fallbacks;
  - without `skyrim-mp/fonts/` the log warns for each file and the window is
    in the system font; without system fonts both faces still load;
  - after a frame the font stack is empty and the current font is the
    overlay's.

  The `xmake.lua` deploy step and a file that is not a font were not run.
- **Item picker and preview:** built on Windows; in game the picker lists names
  and the preview shows models. The attach pass for models that load in the
  background has not run in game yet. On Linux, `ItemCatalog.cpp`,
  `ItemPicker.cpp`, and the settings files ran headless with a fake catalog and
  a stand-in preview: per-key filters, multi-word and case-insensitive search,
  the preview following the hovered row, Enter picking the first match, Escape
  closing the list before the window, Apply saving `Plugin|0xID`, and
  `ItemPreview = false` hiding the preview. The reset button as an icon shown
  only off the default is not built on Windows or run in game. `ItemForms.cpp` and `ItemPreview.cpp`
  need CommonLibSSE-NG and were only checked in game.

## Scope

The plugin's own UI layer: a status panel, chat, the settings window, and the
reusable item picker with a 3D preview. `ui/` never changes the game; it reaches
the network only through the feature modules it presents, such as `ChatSync`,
and the configuration through `Config`.

## Technology

| Option | Result |
| --- | --- |
| Dear ImGui on Skyrim's D3D11 device | **Chosen.** One static library; its only runtime files are the two optional [themed fonts](#themed-faces). Immediate-mode code keeps a settings screen next to the values it edits, and many SKSE plugins draw this way, so it gets along with ENB, ReShade, and Community Shaders. |
| Scaleform (Flash) menus | Native look, but needs SWF tools, ActionScript, and a Papyrus/ESP or SkyUI MCM for settings: much tooling and cross-language glue for a few widgets. |
| Chromium Embedded Framework | Any web UI, but a runtime over 100 MB with separate processes, offscreen texture upload, and fragile Wine/Proton behavior. |

A Scaleform HUD can still be added beside the ImGui settings later without
touching the network side.

## Components

```text
src/ui/
  Overlay.h/.cpp              # renderer hook, ImGui context and backends, window registration
  Fonts.h/.cpp                # system fonts with fallbacks, shipped themed faces
  Theme.h/.cpp                # look of the interactive windows: accent, rounded borders
  Window.h                    # interface every window implements
  WindowManager.h/.cpp        # registered windows, draw order, input focus
  InputCapture.h/.cpp         # input dispatch hook: focus keys, routing to ImGui
  Layout.h                    # placement helpers (screen corners)
  hud/StatusPanel             # connection, ping, traffic, plugin time
  chat/ChatWindow             # chat history and input
  settings/SettingsWindow     # plugin settings: sidebar, section pages, footer
  settings/ConnectionControls # connection button, the line under it, one-line summary
  widgets/Controls            # switch, accent and red buttons, icon buttons
  widgets/ItemPicker          # item dropdown
  preview/ItemPreview         # 3D item capture
src/sync/chat/ChatSync        # channel prefixes, send, rejections as notices, lines to display
src/runtime/PluginTime        # plugin time per pass
assets/fonts/                 # Sovngarde Light and Bold with their license, deployed beside the DLL
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

#### Themed faces

The [themed](#theme) windows use Sovngarde 8.9 by mjorka, a serif after
Skyrim's title lettering, in two more atlas fonts: `Fonts::GetText` is
`Sovngarde Light.ttf` and `Fonts::GetHeading` is `Sovngarde Bold.ttf`. The
overlay's own font stays the atlas default, so the status panel and chat keep
the system font.

- The files are in `assets/fonts/`, unmodified from the
  [standalone release](https://mjorka.itch.io/sovngarde), with
  `Sovngarde OFL License.txt`, and are read from `skyrim-mp/fonts/` under
  `Config::GetDirectory` ([Configuration](configuration.md#file));
  `xmake.lua` copies the folder to every deploy directory after a build.
- They are separate files rather than data in the DLL because Sovngarde is
  under the SIL Open Font License 1.1 and the plugin under GPL-3.0: beside
  the DLL the font stays a work of its own under its own license. The license
  asks that its text and the copyright notice go with every copy, so the
  text file is deployed with the fonts and belongs in any release archive;
  the notice is also in each font's name table. It has no reserved font name.
- Each face is a font of its own with the system files that loaded merged
  after it, in the same order, so a character Sovngarde lacks comes from the
  system font at the same size. Sovngarde has ASCII, most of Latin-1, the
  Polish letters, and the Russian alphabet with Ukrainian `Є`, `І`, `Ї`; it
  lacks `Ґ`, the Turkish and most other Latin Extended-A letters, and `…`,
  `–`, `—`.
- Sovngarde's letters are small for its line height: its x-height is 0.36 of
  ascent plus descent, where Arial's is 0.47. `kShippedGlyphScale`, 1.25,
  goes on the Sovngarde source as `ImFontConfig::ExtraSizeScale`, which
  enlarges glyphs and advances and leaves the line height, so text is as wide
  as in the system font and layouts in font sizes hold. Descenders and
  accented capitals reach about a pixel outside the line at 16 px.
- A file that is missing or is not a font logs a warning and leaves its
  accessor null; `ImGui::PushFont` then keeps the current font, so the
  window shows in the system font.

### Theme

The overlay's base style is `StyleColorsDark` with 4 px window rounding and no
border, which the status panel and chat use. `UI::Theme::Scope` gives every
window and popup begun while it lives the look of the plugin's interactive
windows, by pushing ImGui style colors and variables and popping them when it
ends. `SettingsWindow` wraps its window in one, which covers the lists and
tooltips opened from it, the item picker's included.

- Text is `Fonts::GetText`, pushed at the size in effect, so every size given
  in font sizes is unchanged. Headings push `Fonts::GetHeading` with their own
  size ([Themed faces](#themed-faces)).

- The accent is `Theme::kAccent`, `FF9000`. It marks what the player can act on
  or has selected: the border of the window and its popups, the selected
  section, a switch that is on, the selected and hovered list rows, hovered and
  pressed buttons, fields, and scrollbar grabs, selected text, and the fill of
  the button a view leads to (`Controls::PrimaryButton`, with
  `Theme::kOnAccent` text). Body text is never the accent, so amber warnings
  stay distinct.
- A state is `Theme::kPositive` (green), `kWarning` (amber), or `kNegative`
  (red): the connection indicator, the picker's warnings, values that are not
  applied, refusals, and save results. Red also fills the button that ends
  what is running (`Controls::DangerButton`, with `Theme::kOnAccent` text).
- Surfaces are neutral and near black. Fields and buttons are a white tint
  (`Theme::Tint`) with no border of their own, text is off-white, secondary
  text is `ImGuiCol_TextDisabled`, and separators are a faint white line.
  Popups are opaque, so the rows under an open list do not show through it.
  The resize grip is invisible until hovered.
- Borders are 1 px rounded lines: 10 px corners on the window, 8 px on popups
  and cards, and 6 px on fields, buttons, and grabs. Padding is 10 × 6 px in a
  field and 12 × 10 px in a popup, with 8 × 8 px between items, and the
  scrollbar is 10 px wide.
- Those sizes are for the 16 px base font and are stored in font sizes, then
  floored to whole pixels and kept at 1 px or more, so they follow the overlay
  scale and a border never disappears at a small one.

`UI::Controls` holds the small widgets the themed windows share, each as high
as a frame: `Toggle`, a switch whose knob slides over 0.12 s of frame time;
`PrimaryButton` and `DangerButton`, which differ only in their fill; and
`IconButton`, whose icons (close, reset) are drawn as lines, so they do not
depend on a font having the glyph.

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
| Not connected, whatever the reason | `Disconnected` | red |
| Connected, no sample yet or timed out | `Ping -- ms` | grey |
| ≤ 80 ms / ≤ 150 ms / > 150 ms | `Ping N ms` | green / yellow / red |

While connected, two grey lines follow with the received and sent traffic in
KB/s ([Network](network.md#traffic)), `--` before the first window. Always
shown below: `Plugin N.NN ms`, or `Plugin -- ms` before the first window.

The panel is a debugging readout, not player guidance. It does not say why a
connection is down or that a display name is missing: those need a sentence and an action in the settings window, so the messages are
in its General section ([Connection controls](#connection-controls)).

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
| `@Name text` | whisper | one player, by display name |

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
  picked. The close button at the page's top right or Escape also closes it;
  Escape first goes to an active text field or open combo or list. Any menu
  taking input closes it too.
- Every opening starts from the live configuration; closing discards unapplied
  outfit edits and text still being typed. The section that was open stays
  open, except that an opening with no display name applied shows General
  with the keyboard in its field.
- Centered, 48 × 32 font sizes and at least 40 × 20, in the [theme](#theme),
  without a title bar: it is dragged by any empty part, resized from its lower
  right corner, and uses the mouse.
- A sidebar 11.5 font sizes wide, on a faint panel, lists one item per file
  section in schema order under the title `Multiplayer`: General, which opens
  first and is the display name beside the
  [connection button](#connection-controls), then
  Network, Default Outfit, Performance, and Interface (`[UI]`). The open
  section's item has an accent tint and an accent mark on its left. The
  connection status stays at the sidebar's bottom in every section
  (`ConnectionControls::DrawSummary`).
- The sidebar's title, the open section's title, and the group names inside a
  section are in the heading face; everything else is in the text face.
- The page beside it shows the open section: its title, the section's comment
  as one muted line where the schema has one, and its settings. Only the
  settings scroll, with the scrollbar along the window's edge; the title and
  the footer stay in view.
- Widgets come from the same `Config::Describe` visitor that reads and writes the
  file, so a new setting appears without UI code. A row is the key as words
  (`SpawnsPerSecond` is `Spawns Per Second`) and, in the right 58% of the row,
  its control: a switch at the right edge for a bool, a number field with step
  buttons for an unsigned value, a text field for a string, an item picker for
  a form, and a combo for a choice or key. Group comments are small muted
  capitals above their rows. `[General] DisplayName` is the one setting not
  drawn as a row ([Connection controls](#connection-controls)).
- A setting's comment is not drawn; it is the tooltip of the row's label, and
  of a text field while its value is invalid.
- A number's step buttons move it by a hundredth of its maximum, and typing
  sets any value up to the maximum. Where the schema names zero
  ([Configuration](configuration.md#schema)), a zero value shows that name
  (`Auto`, `Unlimited`) in the empty field, and clearing the field sets zero.
- Edits in every section but Default Outfit take effect and are saved as they
  are made, with no button: a switch or combo at once, a number when its field
  or step button is released, and text when editing ends, on Enter, on a click
  elsewhere, or on picking another section, so a half-typed endpoint never
  reconnects. Escape in a text field puts
  back the text it had when editing started. Text that does not parse turns red
  and leaves the setting at its last valid value, while other edits still
  apply. These sections have no footer; a red line appears below the settings
  only while values in red are not applied or the file could not be saved.
  The display name reports its own text under its field instead.
  Their settings have no way back to the built-in defaults other than editing
  them or removing the line from the file.
- Default Outfit keeps its edits until Apply, since an outfit is put together
  from several picks and each apply can publish new equipment and appearance
  revisions to peers. Its footer has Defaults, enabled while the edits differ
  from the built-in outfit, then Revert and Apply, enabled while they differ
  from the live outfit; Apply then has the accent fill. Revert returns to the
  live outfit and Defaults to the built-in one, without applying. All three
  leave the other sections alone, and an edit applied in another section leaves
  pending outfit edits pending. The footer's left reports unapplied changes, a
  successful save, or a failed one.
- `SettingsWindow` holds one draft `Config::PluginConfig`. After drawing, it
  applies the live configuration with the draft's other sections whenever they
  differ and no number field is active, and Apply does the same with the
  draft's `defaultOutfit`.
- The page is drawn before the sidebar, in one `Config::Describe` pass that
  also collects the section names the sidebar lists. A picked section
  therefore shows from the next frame, after a text field edited until the
  press has seen its editing end.

### Connection controls

General is one step: the name peers see, and joining with it. `SettingsWindow`
draws `[General] DisplayName` as the label `Display Name` over one line of its
text field and the connection button, as multiplayer clients lay out joining
a server, and `ConnectionControls` draws the button (`DrawButton`) and one
line under the two (`DrawNotice`). The section has no status, server name, or
ping of its own: the sidebar's summary and the status panel show those.

- The field takes the page's width less the button, reads
  `Enter a display name` while empty, and applies like any text setting, when
  its editing ends. Applying it never connects, on Enter or otherwise.
- The field is editable only while `Network::GetConnectionState` is
  `kDisconnected`, because a session takes its display name from the hello
  ([Network](network.md#sessions)). Otherwise it is disabled, and hovering it
  shows why (`ConnectionControls::GetDisplayNameLock`):
  `Cancel the connection to change the display name` while connecting, and
  `Disconnect to change the display name` while connected or disconnecting.
- Opening the window with no display name applied puts the keyboard in the
  field. Escape then leaves the field, and a second press closes the window.

The button is the only control that connects. It follows
`Network::GetConnectionState`, so its label never claims a state the transport
has not reached, and keeps its width and identity when the label changes:

| State | Button | Fill | Press |
| --- | --- | --- | --- |
| Connected | `Disconnect` | red | stops wanting a connection |
| Connecting | `Cancel` | plain | stops wanting a connection |
| Disconnecting | `Disconnecting...`, disabled | plain | — |
| Disconnected, the field holds a display name | `Connect` | accent | wants a connection |
| Disconnected, the field is empty or its text is invalid | `Connect`, disabled | plain | — |

`Connect` goes by the field's text, not by the applied display name: pressing
it ends the field's editing, which applies the text before the press lands,
so a name that was only typed connects with one click.

The line under the field and the button is wrapped at the page's width and
shows the first of these that holds. It is the only place the plugin shows
them:

| Condition | Line |
| --- | --- |
| The field's text is not a display name | `A display name cannot start or end with a space or be longer than 128 bytes`, red. The page's footer line does not count this field |
| Disconnected because the server refused the connection, for its display name (`kDisplayNameTaken`, `kDisplayNameInvalid`) or otherwise (`kProtocolMismatch`, `kSessionReplaced`) | the sentence of `Network::GetConnectionRefusal`, red, until `Connect` is pressed |
| Otherwise | the setting's comment, `Shown to other players above your character and in chat.`, muted |

The sidebar's summary (`ConnectionControls::DrawSummary`) is an indicator and
the status on one line, with the ping in muted text at the right while
connected:

| State | Indicator | Status |
| --- | --- | --- |
| Connected | green | `Connected` |
| Connecting | amber, pulsing | `Connecting...` |
| Disconnecting | grey | `Disconnecting...` |
| Disconnected, refused by the server | red | `Disconnected` |
| Disconnected | grey | `Disconnected` |

A game that starts with `AutoConnect` and a display name connects by itself;
nothing else connects without a press. A refusal leaves the player
disconnected with the field editable, and `Connect` then retries with whatever
is applied, which clears the sentence. The refusals are in
[Network](network.md#connection).

Not wanting a connection closes it and removes every remote player until
Connect. The button acts on the live settings, which a display name or endpoint
reaches when its editing ends, and applying an endpoint while connected moves
the connection to it ([Network](network.md#connection)).

## Item picker

`UI::ItemPicker::Draw(id, value, filter)` draws one form field as a searchable
dropdown of loaded items that fit the setting's filter, instead of typing
`Plugin|FormID`. A window keeps one picker for all its fields: it caches each
field's label until the value changes, and only one list can be open. The
catalog and filters are in
[Appearance and equipment](appearance-and-equipment.md#item-filter-and-catalog).

- The picker has no colors or sizes of its own: it takes the style of the
  window that draws it, the [theme](#theme) in the settings window, and the
  theme's amber for warnings.
- The closed field shows the item's name, `Plugin|0xID (not loaded)` in amber
  for a missing plugin, or the name in amber for an item the filter rejects,
  each with an explaining tooltip. A value for a plugin that is not loaded can
  still be written to the file.
- The open list is 40 × 22 font sizes, with a search field that takes keyboard
  focus and a two-column table of name and plugin. There is no `None` row: a
  form setting always has an item. Only visible rows are drawn
  (`ImGuiListClipper`), and the current value is scrolled into view.
- Clicking a row picks it, Enter picks the first match, Escape closes the list.
- While the value differs from the setting's default, the field narrows for a
  reset icon button beside it, which sets the default; its tooltip names the
  default item.
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
   it and the old one no longer does, as soon as it is picked.
   Chat still opens with Enter and gets no cursor.
5. The General section holds the `Display Name` field with one button beside
   it and a line under them. Opening the window with no display name puts the
   cursor in the field, `Connect` is disabled, and the status panel reads
   only `Disconnected`; typing a name and clicking `Connect` once connects,
   while Enter or a click elsewhere only saves the name. Once connected the
   button is a red `Disconnect`, the field is greyed out and says on hover to
   disconnect first, and the sidebar reads `Connected` with a green indicator
   and the ping; Disconnect makes it an orange `Connect` with the field
   editable and the sidebar `Disconnected` with a grey indicator, removes
   remote players, and keeps them away through a save load until Connect.
   Connect right after Disconnect shows a plain `Cancel` and a pulsing amber
   indicator until the connection opens, never `Disconnect` while
   disconnected. A display name another player holds shows a red indicator and
   the refusal's sentence under the field beside `Connect`, and only
   `Disconnected` in the status panel; entering another display name and
   clicking `Connect` connects and clears it. A name ending in a space turns
   red with the reason under the field and disables `Connect`. The sidebar
   shows the same status in every section.
6. General, Network, Performance, and Interface have nothing below their
   settings. Switching `Status Panel` off hides the panel at once; typing a
   display name changes nothing until Enter or a click elsewhere, which saves it
   without connecting; an endpoint without `http://` turns red and the
   connection stays. `View Radius` reads `Auto` and `+` sets 1000. Each change
   is in `skyrim-mp.ini` without pressing anything. Default Outfit shows
   Defaults, Revert, and Apply; a picked item shows a reset icon and reaches
   peers only on Apply, and Defaults leaves the display name and endpoint as
   they are.
7. The picker lists only matching base-game items and races. Hovering rows
   switches the model without flicker on the world behind; background loads
   appear after the attach pass (a stall logs `Item preview waiting`); nothing is
   left on screen after the list closes; the inventory shows its own models
   normally afterwards; FPS stays steady while a list is open;
   `ItemPreview = false` removes the panel.

8. The settings window and the lists it opens have a rounded `FF9000` border
   and no blue: the open section's sidebar item, switches that are on, `Connect`
   and a pending `Apply`, and hovered or pressed widgets are orange at 1080p
   and 4K. Picking a sidebar item shows that section, and the window can be
   dragged by its sidebar or title. The status panel and chat look as before.
9. The settings window, its lists, and its tooltips are in Sovngarde, with the
   sidebar's title, section titles, and group names bold, at 1080p and 4K; the
   status panel and chat stay in the system font. The log has one
   `UI overlay: font` line per file. A display name with characters Sovngarde
   lacks still shows every character. With `skyrim-mp/fonts/` removed, the log
   warns for each file and the window shows in the system font.

## Known limitations

- The settings window's frame is a plain rounded line rather than the vanilla
  menu frame: that art is vector shapes inside the game's SWF files, which the
  overlay cannot draw.
- `SettingsKey` is picked from a list rather than by pressing the key:
  `InputCapture` forwards only editing and navigation keys to ImGui, and the
  current key closes the window.

- A script none of the system fonts cover shows placeholders; emoji cannot be
  typed.
- In a themed window, a character Sovngarde lacks is drawn in the system font,
  so a word can mix the two faces ([Themed faces](#themed-faces)). Sovngarde's
  `1` and `l` look alike, which shows in the server endpoint.
- Editing shortcut letters (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY positions,
  not the keyboard layout. Chat text cannot be selected with the mouse.
- A whisper cannot address `Ulf` with a message starting with `Stone` while a
  player named `Ulf Stone` is online, since the longest name wins.
- The picker covers armor, weapons, torches, ammunition, and races, not spells
  or actors; `ItemKind` is where more kinds would go.
