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
- **Input ahead of other plugins:** not built on Windows or run in game. That
  Modex 3.0.0 and Open Animation Replacer 3.2.1 detour the same call was read
  from their binaries, and that Modex does it after this plugin from
  `skse64.log`. On Linux, `InputCapture.cpp` and `WindowManager.cpp` ran
  against stubbed engine input and a stand-in call site with plugins detouring
  it before and after the hook: a later plugin sees only the dispatch it took
  over; after that neither plugin nor the game sees the focus key or keys
  typed into the chat, and each sees an unfocused key once; a later plugin
  that swallows input still does; the ninth detour is refused with one
  warning.
- **Chat:** not built on Windows or run in game. Server unit tests cover
  routing, the roster, and whisper addressing ([Network](network.md#status)).
  On Linux, with g++ 16 and a logging stub:
  - `ChatRoster.cpp`, `ChatDraft.cpp`, `ChatComposer.cpp`, and `ChatSync.cpp`
    ran against a stubbed transport: draft syntax for every prefix and
    command; names compared without case in ASCII, Latin Extended-A, and
    Cyrillic; the order of the name list; the longest-name rule; a prefix or
    command switching the target as it is typed; a name binding at its space
    only when no longer name continues it; `/r`, `/who`, `/help`, an unknown
    command; Tab order with and without partners; a partner under another
    slot found by name and one who left reported offline; sent lines recalled
    with their targets; the notices for a player who left, text that cannot
    be sent, no connection, and the server's `unknown recipient`.
  - `ChatWindow.cpp`, `ChatHistory.cpp`, and `ChatInput.cpp` ran with those,
    `Theme.cpp`, `MenuFrame.cpp`, and `Fonts.cpp` in a headless Dear ImGui
    1.92.9 context against a folder laid out as the deploy step leaves it,
    with simulated keys and mouse and the draw data rasterized to images, at
    1080p, at 4K, with `BorderColor = Accent`, and without `skyrim-mp/`:
    typing, `!`, Tab, Shift+Tab, and Backspace; the name list by
    keys and by a click; Escape closing the list, then the chat, with the
    text kept and discarded; the command list; Up and Down; each filter
    turning its channel off and on by itself, in the open chat and among the
    closed chat's lines, with the send target unchanged; the unread dots; a
    target and the player's own line turning a channel back on; every filter
    off leaving the notices; a click on a sender's name; the chip's menu; a
    line's menu and the clipboard; a click outside; a whisper to a player who
    left; scrolling by the wheel while the cursor is hidden and a line
    arriving while scrolled back; the closed chat's lines with every channel
    on, with Global off, and with Whispers off. In the images the field, the
    chip, the list, and both menus have the [theme](#theme)'s surfaces,
    corners, and accent, each filter is a chip, filled in its channel's color
    when shown and a grey outline when off, with a rule between the filters
    and the lines, and the filters, lines, chip, list, and recent lines start
    in one column.
    A line's label, up to and with its colon, is in Sovngarde Bold on the
    baseline of the text, in the open chat and the recent lines; without the
    fonts it is in the system font. ImGui's style-stack checks passed.
  - `InputCapture.cpp` and `WindowManager.cpp` ran against stubbed engine
    input: Tab reaching Dear ImGui only for a window that uses it; the chat's
    cursor hidden until the mouse moves, then at the window's point, with the
    wheel and clicks following it; the settings window's cursor in the middle
    at once.

  - The chat's size and wrapped lines ran in that harness over a stubbed
    `Config::Apply`, at 720p, 1080p, and 4K, with `BorderColor = Accent`, and
    without `skyrim-mp/`: a line longer than its row starting beside its
    label and continuing from the left edge under it, in the open chat and
    among the closed chat's lines; a first word that only a whole row has
    room for starting the next row; a word wider than a row, with and
    without punctuation, after other words, and in Cyrillic, starting beside
    its label and filling each row to its end; the resize cursor over the
    top edge, the right edge, and their corner; a drag on each, a few pixels
    a frame, changing the window's size from its lower left corner in every
    frame, with the dragged edge as far from the cursor as when it was
    pressed, the field and the chip where they were in the window, and the
    latest line at the lines' bottom while rows wrap elsewhere, after the
    first frame of a drag that shrinks it; a release
    between steps settling on whole lines and font sizes, saved once and not
    before; a click on an edge saving nothing; a drag inward stopping at 24
    by 9 with the 8-row name list under the filters, and one past the
    screen's corner at 100 by 40 with the chat on the screen; the latest line
    staying in view through each, a line that arrives in the frame after
    the rows got fewer or after a drag's first frame scrolled to, lines
    arriving until the scrollbar shows keeping the latest in view, and
    scrolled-back lines staying where they were; the keyboard back in the field after a drag; a drag that
    loses focus saved; the closed chat's box as wide; the chat opening at
    the saved size, and a new chat having it from the first frame it shows.
    Without the frame's shape the name list reaches 5 px over the filters at
    the smallest size. `SettingsWindow.cpp` ran with the real schema, and
    its Interface section has no row for either key.
  - Limiting the second layout of the lines to a new wrap width
    ([Size](#size)) ran in a smaller harness at 1080p, with Dear ImGui's
    default font, the real menu frame, and the chat's sources, the bullet
    above having run before it: per frame of a drag up, down, wider,
    narrower, and from the knot, the edge following the cursor and the
    field, the chip, and the latest line still, after a shrinking drag's
    first frame, where the latest line was 3 px low; lines arriving after
    such frames and until the scrollbar shows scrolled to; scrolled-back
    lines kept. With 300 lines, a frame took about 135 µs idle, 138 µs
    dragging the height, and 231 µs dragging the width, against 238 µs and
    230 µs for both drags before.
  - An earlier drag ran in game, runtime not recorded: it moved in whole
    font sizes and lines with the window as high as its content, and the
    player saw it resize at a lower frame rate than the settings window,
    with the field and the chip flickering. The drag described under
    [Size](#size) is not built on Windows or run in game.

  Not run anywhere: the wheel over the lines with the cursor shown, the
  Windows clipboard, and the resize cursors as the overlay draws them in game.
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

  The `xmake.lua` deploy step and a file that is not a font were not run. The
  chat in Sovngarde is under Chat above.
- **Menu frame:** not built on Windows or run in game. `menu-frame.shape` was
  regenerated byte for byte from the `messagebox.swf` in the
  `Skyrim - Interface.bsa` of a Steam Special Edition install, build 24914197.
  Its polygons were checked to be simple, apart from each other, inside
  the silhouette, and clockwise. In draw data for 540p to 4K at
  `BorderThickness` 50, 70, and 100, and at 150, above its limit, read back in
  double precision, the anti-aliased fill
  overlaps its own triangles on 12 to 750 pixels and differs from the fill
  without anti-aliasing, away from its edges, on at most 8. `MenuFrame.cpp`
  ran with `SettingsWindow.cpp`, `ChatWindow.cpp`,
  `Theme.cpp`, and `Fonts.cpp` in a headless Dear ImGui 1.92.9 context against
  a folder laid out as the deploy step leaves it and a stubbed `ChatSync`, the
  draw data rasterized to images:
  - every section of the settings window, its frame color list, and the open
    chat with two lines and with a scrolled history have the frame at 1080p
    and 4K, with the sidebar's title, the connection status, the close button,
    the footer's buttons, the scrollbars, and the chat's lines and input clear
    of the knots;
  - the frame is grey in both windows with the default configuration; picking
    `Accent` in the `Border Color` list applies it and both frames are orange
    from the next frame;
  - the `Border Thickness` slider reads `70%` and has no step buttons; a drag
    past its right end gives 100 and past its left end 50, one to its middle
    75, and the frame and the content inside it keep the previous value while
    the button is held and follow the new one once it is released, at 1080p
    and 4K; Ctrl and a click make it a field, where typing applies nothing
    until Enter, 85 sets 85, 150 gives 100, and 7 gives 50;
  - a drag on the lower right corner resizes the settings window and the frame
    follows, and the cursor over that corner is the resize one; recent chat
    lines have no frame and stand in the open chat's column;
  - without `skyrim-mp/frame/` the log warns and both windows have their
    style's rounded rectangle with a line in the frame color; a file cut
    short, of another version, with a `nan`, with a point
    count out of range, or with no polygons each warn with the reason and fall
    back the same way; a file with CRLF line ends loads.

  The `xmake.lua` deploy step was not run. `[UI] BorderColor` in the file is
  under [Configuration](configuration.md#status).
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
| Dear ImGui on Skyrim's D3D11 device | **Chosen.** One static library; its only runtime files are the two [themed fonts](#themed-faces) and the [menu frame](#menu-frame), all optional. Immediate-mode code keeps a settings screen next to the values it edits, and many SKSE plugins draw this way, so it gets along with ENB, ReShade, and Community Shaders. |
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
  MenuFrame.h/.cpp            # Skyrim's menu frame around a window, from its shipped shape
  Window.h                    # interface every window implements
  WindowManager.h/.cpp        # registered windows, draw order, input focus
  InputCapture.h/.cpp         # input dispatch hook: focus keys, routing to ImGui
  Layout.h                    # placement helpers (screen corners)
  hud/StatusPanel             # connection, ping, traffic, plugin time
  chat/ChatWindow             # open and closed chat, focus, the send target's channel kept shown
  chat/ChatHistory            # lines, channel filters, unread marks, scrolling, the line menu
  chat/ChatInput              # target chip and its menu, text field, name and command list
  chat/ChatSize               # the open chat's size, the drag that sets it, saving it
  chat/ChatStyle.h            # channel and notice colors
  settings/SettingsWindow     # plugin settings: sidebar, section pages, footer
  settings/ConnectionControls # connection button, the line under it, one-line summary
  widgets/Controls            # switch, accent and red buttons, icon buttons
  widgets/ItemPicker          # item dropdown
  preview/ItemPreview         # 3D item capture
src/sync/chat/ChatSync        # send to a target, rejections as notices, lines to display
src/sync/chat/ChatRoster      # connected players by name, completion, whisper partners
src/sync/chat/ChatDraft       # syntax of prefixes and commands
src/sync/chat/ChatComposer    # send target, name and command list, sent lines
src/runtime/PluginTime        # plugin time per pass
assets/fonts/                 # Sovngarde Light and Bold with their license, deployed beside the DLL
assets/frame/                 # the menu frame's shape, deployed beside the DLL
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

The [themed](#theme) windows, the settings window and the chat, use Sovngarde
8.9 by mjorka, a serif after Skyrim's title lettering, in two more atlas
fonts: `Fonts::GetText` is `Sovngarde Light.ttf` and `Fonts::GetHeading` is
`Sovngarde Bold.ttf`. The overlay's own font stays the atlas default, so the
status panel keeps the system font.

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
border, which the status panel uses. `UI::Theme::Scope` gives every
window and popup begun while it lives the look of the plugin's interactive
windows, by pushing ImGui style colors and variables and popping them when it
ends. `SettingsWindow` wraps its window in one, which covers the lists and
tooltips opened from it, the item picker's included, and `ChatWindow` wraps
the open chat and the recent lines in one each. The settings window and the
open chat have the [menu frame](#menu-frame) in place of the style's
background and border.

- Text is `Fonts::GetText`, pushed at the size in effect, so every size given
  in font sizes is unchanged. Headings push `Fonts::GetHeading` with their own
  size ([Themed faces](#themed-faces)).

- The accent is `Theme::kAccent`, `FF9000`. It marks what the player can act on
  or has selected: the border of the window's popups, the selected section, a
  switch that is on, the selected and hovered list rows, hovered and pressed
  buttons, fields, and scrollbar grabs, selected text, and the fill of the
  button a view leads to (`Controls::PrimaryButton`, with `Theme::kOnAccent`
  text). Body text is never the accent, so amber warnings stay distinct.
- A state is `Theme::kPositive` (green), `kWarning` (amber), or `kNegative`
  (red): the connection indicator, the picker's warnings, values that are not
  applied, refusals, and save results. Red also fills the button that ends
  what is running (`Controls::DangerButton`, with `Theme::kOnAccent` text).
- Surfaces are neutral and near black. Fields and buttons are a white tint
  (`Theme::Tint`) with no border of their own, text is off-white, secondary
  text is `ImGuiCol_TextDisabled`, and separators are a faint white line.
  Popups are opaque, so the rows under an open list do not show through it.
- Borders are 1 px rounded lines: 8 px corners on popups and cards, 6 px on
  fields, buttons, and grabs, and 10 px on a window that has no menu frame.
  Padding is 10 × 6 px in a field and 12 × 10 px in a popup, with 8 × 8 px
  between items, and the scrollbar is 10 px wide.
- Those sizes are for the 16 px base font and are stored in font sizes, then
  floored to whole pixels and kept at 1 px or more, so they follow the overlay
  scale and a border never disappears at a small one.

`UI::Controls` holds the small widgets the themed windows share, each as high
as a frame: `Toggle`, a switch whose knob slides over 0.12 s of frame time;
`PrimaryButton` and `DangerButton`, which differ only in their fill; and
`IconButton`, whose icons (close, reset) are drawn as lines, so they do not
depend on a font having the glyph.

### Menu frame

The settings window and the open chat are framed as the game frames its message
boxes: a silhouette with a block at each corner, a line along each side, and
a knot in each corner. `UI::MenuFrame` draws it from a file, so the game's art
is not part of the plugin.

- The art is shape 10 of `Interface/messagebox.swf` in the game's
  `Skyrim - Interface.bsa`. `menu-frame.shape` is FFDec's SVG export of the
  shape with its curves flattened to within 0.03 px: the outline of the
  silhouette and the 20 line pieces, 526 points, each polygon clockwise. The
  file is the source; nothing in the repository regenerates it.
- A point is measured from the two sides of the framed rectangle nearest to it
  across the sprite's scaling grid (`lt`, `rt`, `lb`, `rb`, then two lengths in
  the art's pixels). Drawing at any size is then one subtraction per
  coordinate: the corners keep their shape and the lines stretch, as the
  game's 9-slice does. The file also gives `inset`, 8.2, how far the lines
  reach in from a side, and `corner`, 27.05, how far the knots do.
- The file is `skyrim-mp/frame/menu-frame.shape` under `Config::GetDirectory`,
  deployed by `xmake.lua`. It is a file rather than data in the DLL for the
  reason the [themed faces](#themed-faces) are: the art is Bethesda's and the
  plugin is under GPL-3.0.
- `MenuFrame::Load` runs once when the overlay initializes. It accepts
  `frame 1` as the first line, then `inset`, `corner`, and polygons as
  `backing N` or `line N` followed by N points; lengths must be finite and
  within 0 to 4096, a polygon has at least 3 points, and a file at most 8192.
  A file that is missing or malformed logs a warning with the reason and leaves
  the shape empty.
- `MenuFrame::Draw(line, backing)` fills the current window's rectangle: the
  silhouette in the backing color, then the pieces in the line color, each with
  `ImDrawList::PathFillConcave`, which anti-aliases the edges. The window is
  begun with `ImGuiWindowFlags_NoBackground`, and the clip rectangle is the
  whole window, since a window's own leaves out part of its padding. It costs
  526 points and 21 small triangulations per framed window per frame and
  allocates nothing once the draw list has grown.
- The line color must be opaque. The anti-aliased fill moves each point half a
  pixel inwards, which on pieces this fine turns some triangles over others;
  an opaque color hides the overlap and a translucent one shows it as brighter
  specks.
- A pixel of the art is 1.5 / 16 of the font size times `[UI] BorderThickness`
  ([Configuration](configuration.md#ui)), a percentage from 50 to 100 read
  each frame. The game lays its menus out for 720p, so 100 is the size the
  game draws the frame at, at any resolution. The setting has no line weight
  of its own: lines, knots, and inset grow and shrink together. At the default,
  70, the lines are a little over 2 px thick at 1080p and end 9 px in from the
  window's edge, and a knot fills 29 px of each corner; at 100 those are 3 px,
  13 px, and 41 px.
- `MenuFrame::GetMetrics` gives a window those two lengths in pixels to lay its
  content out by. A knot stays inside the square of `corner` at its window
  corner, and cuts across it, so content is clear of it outside that square or
  halfway into it along the diagonal.
- The line color is `Theme::GetFrameColor`, which follows `[UI] BorderColor`
  ([Configuration](configuration.md#ui)) each frame, so a pick in the settings
  window shows at once: `Vanilla`, the default, is the game's `999999` as it
  looks at the 80% opacity of the game's message box over the dark backing,
  `7D7D7D`, and `Accent` is `FF9000` as it looks at 85%, `DB7D03`. The backing
  is the window's own background color, and a window's popups keep their
  accent border with either.
- Without a shape, `Draw` fills the style's rounded rectangle and, where the
  style has a window border, outlines it in the line color; the metrics are
  then the style's border width and rounding, so a window lays itself out the
  same way with or without the shape.

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
- Other plugins with ImGui menus, such as Modex and Open Animation Replacer,
  detour the same `call`, and whichever detours last runs first. A plugin
  running before the hook would act on keys a focused window owns, such as
  opening its menu on Delete typed into the chat. After each dispatch the
  thunk therefore compares the call's destination with its own
  (`CodeDetour::ReadCallTarget`) and detours the call again when it differs.
  That leaves the thunk on both sides of the other plugin: its outermost
  call routes the events and the nested ones pass them on, each to the
  destination its detour replaced. Detouring again reuses the trampoline
  entry, so it needs no more trampoline bytes. After 8 detours, or if the
  instruction is no longer a `call`, it stops and logs a warning. The plugin
  that took the call over sees that one dispatch.
- Every event of a key or button goes where its press went: a key held while a
  window takes focus is still released to the game, and a key that closed a
  window is not.
- Key presses and releases become ImGui key events: editing and navigation keys,
  modifiers, and the letters of editing shortcuts (scan codes are physical
  positions). Typed text comes from Skyrim's `CharEvent`s. Tab is forwarded
  only to a window that returns true from `Window::UsesTab`, the chat;
  elsewhere it is dropped, so it cannot move focus off a text field. Gamepad
  input always reaches the game.
- A window that returns true from `Window::UsesMouse` gets mouse movement and
  buttons through ImGui: `InputCapture` clears `NoMouse`, sets
  `io.MouseDrawCursor`, and moves an absolute cursor by DirectInput's relative
  counts, scaled by `backBufferHeight / 1080` and clamped to the back buffer. The
  cursor starts at `Window::GetCursorStart`, or centered without one, whenever
  such a window takes focus, and its position is
  queued after the Win32 backend's, which follows the hidden system cursor. Left,
  right, and middle buttons and the wheel go to ImGui. A keyboard-only window
  gets the wheel as `WindowInput::wheelSteps`.
- A window that also returns true from `Window::HidesIdleCursor`, the chat,
  starts each focus as a keyboard-only one: no cursor, `NoMouse` set, clicks
  dropped, and the wheel as `wheelSteps`. The first mouse movement shows the
  cursor at its start point, without moving it, and the mouse then works as
  above until focus ends.
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
wire, the roster, routing, and server rules are in [Network](network.md#chat).

Goal: talk to everyone, to players nearby, or to one player; always see where
the next line goes and change it with a key, a prefix, a command, or a click;
whisper by a few letters of a name, and keep whispering the same player
without naming it again; tell the channels apart at a glance, and turn each
off and on; see as many lines as the player wants, by dragging the chat larger.
Non-goals: chat
history for late joiners, a saved log, an ignore list, moving the chat, and
staying open after a line is sent.

| Channel | Reaches | Color |
| --- | --- | --- |
| Local | placed players within `chat.local_radius` (8192) in the same worldspace, or in the same interior | lime |
| Global | every connected player | gold |
| Whisper | one player | violet |

`ui/chat/` draws and forwards keys; what they mean is in `sync/chat/`, which
has no Dear ImGui and no engine access.

### Send target

`ChatSync::Composer` holds the target (`ChatSync::ChatTarget`): a channel, and
for a whisper a player by slot and name. It starts as Local, survives closing
the chat, and is not saved. `ChatInput` shows it as a chip left of the text
field, as high as the field and with its corners, in the channel's color over
an 18% fill of it, 30% under the cursor: `Local`, `Global`, or `To <name>`,
the name cut at 12 font sizes. The empty field reads
`Press Tab to switch channel`.

| Input | Effect |
| --- | --- |
| Tab, Shift+Tab | The next or previous of Local, Global, and the 3 latest whisper partners, most recent first. A whisper target that is not among them yet stands after Global |
| `!` as the first character | Global |
| `@` as the first character | Opens the [name list](#name-list) |
| `/l`, `/g` (`/local`, `/global`) | Local, Global |
| `/w <name>` (`/whisper`) | As `@` |
| `/r` (`/reply`) | The sender of the latest received whisper; `Nobody has whispered you yet.` without one |
| Backspace in the empty field | From a whisper back to the channel it replaced |
| A click on the chip | A menu of Local, Global, and the 5 latest partners |
| A click on a sender's name, or `Whisper` in a line's menu | A whisper to that player |

- A prefix or a command is removed from the text when it is complete: `!` and
  `@` at once, a command when a space follows it or on Enter, where the chat
  then stays open. `ChatDraft::Interpret` decides that from the text alone,
  comparing command names without case, and `Composer::OnEdited` applies it
  after every edit, so the chip is always where Enter sends.
- `/who` adds a notice with the number of connected players and `/help` one
  line per command. An unknown command is a notice on Enter and is not sent,
  so text that starts with `/` never is.
- `ChatRoster` keeps the whisper partners, at most 8, moved to the front by
  every whisper sent or received except one to oneself, and compared by name
  as well as slot, since a player who connects again has another slot.

### Name list

While the text starts with `@` or `/w `, a list above the field offers the
connected players whose names start with what follows, at most 8.

- `ChatRoster` copies the roster from the transport when its revision changes
  ([Network](network.md#client)) and sorts it by folded name. `Complete`
  returns the player named exactly as typed, then whisper partners, then
  players the client currently receives (`Network::CopyNearbyPlayers`), then
  the rest by name, without the local player. It runs when the text changes,
  not per frame.
- Names are folded by a table for ASCII, Latin-1, Latin Extended-A, Greek,
  and Cyrillic, and compared byte for byte otherwise. The server's lower-casing
  is not needed here: both sides of every comparison are folded the same way.
- Up and Down move the selection; Tab, Enter, or a click accept it, which sets
  the target and empties the field, and the chat stays open. Escape hides the
  list until the text changes.
- A name typed in full binds at its space once no other name continues what
  is typed: with `Ulf` and `Ulf Stone` online, `@Ulf ` still lists
  `Ulf Stone`, and `@Ulf h` whispers `Ulf` with `h` left as the text.
  `ChatRoster::FindAddressee` takes the longest name followed by a space or
  the end.
- With nobody matching, the list is one row, `No player with that name is
  online`, and Enter adds that notice and keeps the text.
- While the text is `/` and letters, the same list offers the commands whose
  names start with them, each with its description; accepting one applies it.

### Sending

- Enter sends the text to the target and closes the chat; Enter in the empty
  field only closes. `ChatSync::Send` trims the text, validates it
  (`protocol/ChatMessage.h`, shared with the parser), and
  `Network::PublishChatMessage` sends `ChatSend` with the channel, the
  recipient's slot for a whisper, and a request ID.
- A whisper resolves its player with `ChatRoster::Resolve`: the slot while
  that connection lasts, otherwise the connection that has the name now.
- When nothing was handed to the server, the chat stays open with the text
  and the target, and a notice says why: `<name> is offline.`,
  `Not connected to the server; your message was not sent.`, or
  `Your message contains characters that cannot be sent.`
- A rejecting `Result` runs its callback on the transport thread, and each
  server rejection (`rate limited`, `not in world`, `unknown recipient`)
  becomes a notice.
- Up and Down in the field, while no list shows, step through the last 50
  lines sent in this game session, consecutive repeats kept once. A recalled
  line restores its target. Down past the newest returns the text and target
  that were being written.

### Lines and filters

- `ChatSync::DrainLines` returns received messages and notices. A line carries
  its label and the other player: the sender, or the recipient of the local
  player's own whisper, whose label reads `To <recipient>`, where a received
  one reads `From <sender>`. Senders are shown as a player label
  ([Appearance and equipment](appearance-and-equipment.md#player-label)).
- `ChatHistory` keeps the latest 100 lines of each channel and 100 notices,
  each stamped with `ImGui::GetTime()` when it arrived, so a busy channel only
  pushes out its own lines.
- A row above the lines has a filter for each channel: `Local`, `Global`, and
  `Whispers`. Each is a toggle of its own: a click turns its channel's lines
  off or on, in the open chat and among the closed chat's lines, and changes
  no other filter. All are on at first and any number may be off; notices
  show regardless. `ChatHistory` keeps collecting the lines of a channel that
  is off, so turning it on shows what arrived meanwhile.
- A filter is drawn as a chip, so it reads as a switch: its label inside an
  outline with the field's corners, a sixteenth of the font size and at
  least 1 px thick, at 55% of the label's color. Shown, the label is in the
  channel's color over the fill the send target's chip has
  (`ChatStyle::kChipFillAlpha`, 18%, and 30% under the cursor). Off, it is
  the theme's muted text color with no fill, and under the cursor the text
  color with an 8% fill. Over either the cursor is a hand. A faint rule as
  wide as the chat, in the theme's separator color, separates the filters
  from the lines. It is as far under the chips as the inside of the frame's
  line is above them (the window's padding and the frame's, less
  `MenuFrame::Metrics::inset`), so the chips stand midway between the two.
- A filter does not change the send target. A channel that is off turns on
  when the target changes to it, by any input (`ChatWindow`), and when a line
  the local player sent arrives in it (`ChatHistory::Collect`), so where the
  next line goes and what the player said stay in view. The target's channel
  can be turned off again and then stays off until either happens.
- A filter that is off has a dot in its color on its upper right corner while
  lines of its channel arrived that the open chat has not shown.
- A line's label, everything up to and with its colon, takes the channel's
  color and the heading face ([Themed faces](#themed-faces)). Text is the
  theme's, in the text face, and notices are orange. Under the cursor a label is underlined; a right click on a line opens
  `Whisper`, `Copy name`, and `Copy message`, the first two only for another
  player's line.
- A line wider than the chat wraps at words. Its text starts beside its label
  and continues from the chat's left edge under the label, not from where the
  text started, so a long name does not narrow every row (`DrawAfterLabel` in
  `ChatHistory.cpp`), in the open chat and among the closed chat's lines. The
  rows of one line are a font size apart, closer than two lines are.
- `FindRowEnd` ends each row where Dear ImGui's word wrap would
  (`ImFont::CalcWordWrapPosition`), with two differences. A word, text
  between spaces, that only a whole row has room for starts the next row
  whole, also when that leaves the label alone on its row; Dear ImGui would
  cut it beside the label, or break it after punctuation. A word wider than
  a whole row, such as an address or text without spaces, fits no row, so it
  starts where the text before it ends, beside the label too, and is cut at
  each row's end by character (`ImFont::CalcTextSizeA`), leaving no row
  short. Notices have no label and wrap as Dear ImGui does.

### Size

- The open chat's lines are `[UI] ChatWidth` font sizes wide and `ChatLines`
  lines high ([Configuration](configuration.md#ui)): 30 by 10 by default,
  within 24 to 100 and 9 to 40. At 24 the field still has room beside the
  longest chip, and 9 lines keep the 8-row name list under the filters.
  Both are in font sizes, so the chat follows the overlay scale.
- The player drags the open chat's top edge, its right edge, or the corner
  between them. The chat stays in its lower left corner, so it grows upward
  and to the right. The grips are the strips the menu frame runs in, the
  window's padding and the frame's on that side, which hold nothing else to
  click, and the corner is the knot's square (`MenuFrame::Metrics::corner`).
  Over them the cursor is the vertical, horizontal, or diagonal resize one;
  nothing else marks them. `ChatSize::FindEdges` finds them by position:
  Dear ImGui items there would add to the window's content size.
- During a drag the size follows the cursor by the pixel, as the settings
  window's does, so the top row of the lines may show in part. A drag in
  whole steps was dropped: at a font size a step it looked like a low frame
  rate. The size stops at the limits and at what the screen has room for
  with the chat's margin on every side (`GetUnits` in `ChatWindow.cpp`). A
  configured size the screen has no room for is drawn at that room and stays
  in the file.
- Releasing the button settles the size on the nearest whole font size
  across and line (`ChatHistory::GetLinePitch`) up, so the history ends on a
  whole line again, and applies it with `Config::Apply`, once and only if
  it changed, and logs it; a chat that loses focus mid-drag does the same
  (`ChatSize::Release`). `ChatSize` reads the configuration each frame
  outside a drag.
- The window has the dragged size in the frame the cursor moved.
  `ChatSize::Follow` runs before `ImGui::Begin`, and the window is given
  both its width and its height (`ImGui::SetNextWindowSize`): the lines'
  plus what is around them, the filters above and the input row below
  with the padding, as measured in the previous frame
  (`ChatWindow::aroundHeight`). The lines take what the window leaves above
  the input row, so the row stays at the window's bottom. A window as high
  as its content would follow a frame late with the input row off its place
  meanwhile, and one as wide as its content would never get narrower: the
  rule and the input row take the width there is. Only a new chat is as
  high as its content, for the one frame Dear ImGui hides a new window.
  `ChatSize::Grab` starts a drag after `Begin`, where the window's place is
  known.
- While the size changes, the latest line stays at the lines' bottom if it
  was in view (`ChatHistory::following`), though rows wrap elsewhere; lines
  scrolled back stay where they are. Dear ImGui scrolls by what the lines
  took a frame ago. When they wrap at another width, rows above the view
  change too, which would put the latest line off the bottom for a frame
  each time the rows change. In a frame whose wrap width differs, by a drag
  or by the scrollbar showing or going (`ChatHistory::shownWidth`),
  `ChatHistory::DrawLines` therefore lays the lines out twice: once under
  an empty clip rectangle, which draws nothing and takes no click, to find
  what they take now, and then from where that scrolls them to. It passes
  that height to the next frame (`ImGui::SetNextWindowContentSize`), where
  Dear ImGui would otherwise count how far from their place the lines were
  drawn and the chat would stop following.
- A new height alone needs no second layout: the scroll to the end of the
  lines (`ImGui::SetScrollHereY`) is applied at the height of the frame it
  lands in, so through a drag the latest line stays at the bottom. Only in
  the first frame of a drag that shrinks the chat is it below the bottom,
  by the cursor's first move. Estimating the height from Dear ImGui's last
  count instead was dropped: a line arriving or the scrollbar showing in
  that frame makes it wrong, and the chat would stop following.
- So a drag that changes the width costs about one more layout of the lines
  a frame, about 230 µs against 135 µs idle with 300 lines held in a
  Linux benchmark, and one that changes only the height costs nothing more.
- The closed chat's box is as wide as the lines and still shows at most 6.

### Window and input

- Enter or keypad Enter during gameplay opens the chat and focuses its field.
  Typing goes only to the chat; the character does not move or look around, and
  keys already held keep acting until released.
- Escape closes an open menu, else hides the name or command list, else
  discards the text typed since opening and closes the chat. Every key,
  console and menu keys included, goes to the chat while it is open, but a
  menu that opens by itself, such as dialogue an NPC starts or a loading
  screen, closes it and keeps the text.
- The chat uses the mouse, with the cursor hidden until the mouse moves
  ([Input focus](#input-focus)), so a player who only types sees none. It
  appears in the middle of the chat. After any click the keyboard returns to
  the field; a click outside the chat does nothing.
- Open, it sits in the lower-left corner above the magicka bar, with the
  filter row, the lines at the chat's [size](#size), and the input row,
  inside the [menu frame](#menu-frame) over a 60% background. The frame adds half of its
  inset and corner to the padding on
  every side, 19 px at 1080p at the default thickness, which keeps the content
  clear of the knots. The wheel scrolls 3 lines per notch and Page
  Up/Down a page; under the shown cursor the lines scroll as Dear ImGui does.
  New lines scroll down unless the history was scrolled back.
- Closed, the newest lines (at most 6) show for 10 s on a faint background and
  fade out over the last 2; with nothing recent nothing is drawn. They follow
  the filters, whispers included, so turning `Global` off is how a player
  keeps global chat off the screen. They have no
  frame: its corners alone are taller than a line or two. Their box is in from
  the chat's corner by the frame's padding, so the lines stay in the same
  column when the chat opens.
- The open chat and the recent lines are each drawn in a `Theme::Scope`
  ([Theme](#theme)), so the chat has no colors or sizes of its own beyond
  `ChatStyle`'s channel and notice colors: the field is the theme's white
  tint with 6 px corners, the scrollbar its thin grab, the chip's and a
  line's menus its opaque popups with the accent border, and their hovered
  and selected rows the accent. The name and command list is a child window
  with the popup's background, so it looks the same. Text is Sovngarde Light
  ([Themed faces](#themed-faces)).
- The filters, the lines, the chip, the list, and the recent lines start at one
  column, and the rule, the scrollbar, the list, and the field end at one.
  Lines are a quarter of a font size apart (`ChatHistory::GetLinePitch`),
  closer than the theme's items. The recent lines' box has the theme's 10 px
  corners and no border.
- Dear ImGui details the input relies on: the field has
  `ImGuiInputTextFlags_CallbackAlways`, `CallbackHistory`, and
  `CallbackCompletion`, and its callback is where edits reach the composer
  and rewritten text returns to the field, in the same frame. Escape makes
  Dear ImGui put back the text the field had when it took the keyboard before
  that callback runs, so the callback ignores a frame with Escape pressed and
  the field is refilled from the composer's text. Keyboard focus is requested
  again whenever the field is not active, except while a menu is open or the
  mouse holds an item, whose click it would otherwise take.

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
- Centered, 48 × 32 font sizes and at least 40 × 20, in the [theme](#theme)
  and the [menu frame](#menu-frame) over a 96% background, without a title bar:
  it is dragged by any empty part, resized from its lower corners and its
  edges, and uses the mouse. The sidebar and the page fill what the frame's
  lines enclose. The frame's backing covers ImGui's resize grip, and the line
  ImGui draws along a hovered or dragged edge is transparent
  (`ImGuiCol_SeparatorHovered` and `ImGuiCol_SeparatorActive` around `Begin`),
  so resizing shows the resize cursor only.
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
  settings scroll, with the scrollbar along the frame's line; the title and
  the footer stay in view. The close button and the end of the settings of a
  section without a footer stay out of the knots' squares.
- Widgets come from the same `Config::Describe` visitor that reads and writes the
  file, so a new setting appears without UI code. A row is the key as words
  (`SpawnsPerSecond` is `Spawns Per Second`) and, in the right 58% of the row,
  its control: a switch at the right edge for a bool, a number field with step
  buttons for an unsigned value, a slider for a percentage, a text field for a
  string, an item picker for a form, and a combo for a choice or key. Group comments are small muted
  capitals above their rows. `[General] DisplayName` is drawn beside the
  connection button instead of as a row
  ([Connection controls](#connection-controls)), and a `Layout` setting, as
  the chat's [size](#size) is, is not drawn at all.
- A setting's comment is not drawn; it is the tooltip of the row's label, and
  of a text field while its value is invalid.
- A number's step buttons move it by a hundredth of its maximum, and typing
  sets any value up to the maximum. Where the schema names zero
  ([Configuration](configuration.md#schema)), a zero value shows that name
  (`Auto`, `Unlimited`) in the empty field, and clearing the field sets zero.
- A percentage's slider runs between its limits and shows the value with a
  percent sign. Ctrl and a click turn it into a field, and a typed value is
  clamped to the limits.
- Edits in every section but Default Outfit take effect and are saved as they
  are made, with no button: a switch or combo at once, a number when its field
  or step button is released, a percentage when its slider is released, so the
  frame does not resize the window's content under the cursor mid-drag, and
  text when editing ends, on Enter, on a click
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
  differ and no number field or slider is active, and Apply does the same with the
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
   shows a notice and keeps the chat open with the text.
3. The chip reads `Local` at first, and `hi` reaches players within 8192 units
   or in the same interior with a lime name, and not a player farther away.
   `!` as the first character turns the chip to `Global` and is not part of
   the text; the line reaches every player with a gold name, and so does the
   next one, until Tab or `/l`.
4. `F10` opens the settings window with a cursor, and the game receives no input
   until it closes; `F10` again closes it. A new `SettingsKey` opens and closes
   it and the old one no longer does, as soon as it is picked.
   Chat still opens with Enter and shows no cursor until the mouse moves.
   With Modex installed and bound to Delete, Delete and letters typed into the
   chat or a settings field do not open its menu, and Delete opens it once
   the window is closed; `skyrim-mp.log` has one line about detouring input
   dispatch again.
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

8. The settings window has no blue: the open section's sidebar item, switches
   that are on, `Connect` and a pending `Apply`, and hovered or pressed widgets
   are orange at 1080p and 4K, and the lists it opens have a rounded `FF9000`
   border. Picking a sidebar item shows that section, and the window can be
   dragged by its sidebar or title. The status panel looks as before.
9. The settings window, its lists, and its tooltips are in Sovngarde, with the
   sidebar's title, section titles, and group names bold, at 1080p and 4K. The
   open chat, with its filters, field, name list, and menus, and the recent lines
   are in Sovngarde too, with each line's label, up to its colon, bold,
   and the lines stay in the same column and at the same
   height when the chat opens; the status panel stays in the system font. The
   log has one `UI overlay: font` line per file. A display name or chat line
   with characters Sovngarde lacks still shows every character. With
   `skyrim-mp/fonts/` removed, the log warns for each file and the window and
   the chat show in the system font.

10. The settings window and the open chat have the game's menu frame in the
    game's grey, with a knot in each corner, at 1080p and 4K; nothing in
    either window touches a knot, and resizing the settings window keeps the
    knots in its corners. The `Border Thickness` slider reads `70%`; dragging it
    to its right end, 100, makes the frame as large as on a message box of the
    game once released, its left end is 50, and at each the content stays
    inside the lines and clear of the knots.
    Picking
    `Accent` for `Border Color` in the Interface section turns both frames
    `FF9000` at once and writes `BorderColor = Accent` to `skyrim-mp.ini`;
    `Vanilla` turns them grey again. Recent chat lines have no frame and do
    not move sideways when the chat opens. The log has one
    `UI overlay: menu frame` line. With `skyrim-mp/frame/` removed, the log
    warns and both windows have a rounded line in the frame color.
11. With `Ulf Stone` online, `@u` lists it; Tab or Enter turns the chip to
    `To Ulf Stone` and empties the field, and a line reaches only that player,
    reading `To Ulf Stone` for the sender and `From <sender>` for the
    recipient in violet. Enter, text, Enter whispers the same player again.
    `/r` after a received whisper targets its sender, and Backspace in the
    empty field returns to the channel before. A name nobody has shows one row
    saying so. Once the player disconnects, a line adds
    `Ulf Stone is offline.` and the chat stays open with the text.
12. Up in the field recalls the last sent line with its target, and Down
    returns the text that was being written. `/` lists the commands; `/who`
    shows the number of players online; `/dance` shows a notice and sends
    nothing.
13. Moving the mouse in the open chat shows a cursor in the middle of the
    chat, and the character does not turn or attack. A click on another
    player's name turns the chip to that player; a right click on a line and
    `Copy message` puts its text on the clipboard; a click on the chip lists
    Local, Global, and the latest partners. Typing continues after each click,
    and a click outside the chat changes nothing.
14. The filters `Local`, `Global`, and `Whispers` are all on at first. A
    click on `Global` turns its chip grey and leaves the other two and the
    send target as they were; a global line then shows neither in the open
    chat nor among the closed chat's lines, and `Global` gets a dot on its
    corner. With `Whispers` off a
    whisper stays out of the closed chat too. A second click shows the
    channel again with the lines that arrived meanwhile. With `Global` off,
    typing `!` turns it on, and so does the player's own global line coming
    back. With all three off only notices show.
15. The open chat has no blue and no square corners, at 1080p and 4K: the
    field is a faint rounded tint, the chip is rounded in its channel's
    color, the name list and both menus have a rounded `FF9000` border with
    orange hovered and selected rows, and the scrollbar is thin. Each filter
    is a rounded chip, filled and outlined in its channel's color when shown
    and a grey outline when off, and a rule under the filters separates them
    from the lines. The filters, the lines, and the chip start at one column,
    and the rule and the field end at one.
16. A chat line wider than the chat continues from the left edge under its
    sender's name, in the open chat and among the closed chat's lines. Over
    the open chat's top edge, right edge, and upper right knot the cursor is
    a resize one; a drag grows or shrinks the chat from its lower left
    corner with the cursor, as smoothly as a drag resizes the settings
    window, between 24 by 9 and 100 by 40 or what the screen has room for.
    Throughout, the filters, the field, and the chip keep their place in the
    window without flickering and the latest line stays at the bottom. On
    release the chat settles on whole lines and font sizes. After a drag
    `skyrim-mp.ini` has `ChatWidth` and `ChatLines`, the closed chat's lines
    are as wide, the chat has that size after the game restarts, and the
    Interface section shows neither key.

## Known limitations

- Lists, tooltips, and recent chat lines have no menu frame: its corners need
  about 65 px of height at 1080p at the default thickness. The settings window's
  resize grip and edge highlight and the chat's grips are not drawn, so only
  the cursor shows where each resizes.
- `BorderThickness` sizes the whole frame; thinner lines with knots of the same
  size would need another shape. Below about 60 the knots are hard to read at
  1080p.
- The frame is exported ahead of time from the vanilla `messagebox.swf`, not
  read from the player's game, so a mod that replaces the game's menu art does
  not change it.
- `SettingsKey` is picked from a list rather than by pressing the key:
  `InputCapture` forwards only editing and navigation keys to ImGui, and the
  current key closes the window.
- A focused window keeps its keys only from plugins that take them from the
  input dispatch call. One that reads the keyboard another way, such as
  `GetAsyncKeyState`, the window procedure, or DirectInput, still sees them.
- Another plugin's menu that neither pauses the game nor pushes an input
  context does not block focus keys, so a focus key typed there opens the
  window over it.

- A script none of the system fonts cover shows placeholders; emoji cannot be
  typed.
- In a themed window, a character Sovngarde lacks is drawn in the
  system font, so a word can mix the two faces
  ([Themed faces](#themed-faces)); in chat that is any line in a script other
  than Latin or Russian and Ukrainian Cyrillic. Sovngarde's `1` and `l` look
  alike, which shows in the server endpoint.
- Editing shortcut letters (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY positions,
  not the keyboard layout. Chat lines cannot be selected with the mouse; a
  line's menu copies its text or its sender's name.
- `@Ulf Stone cold` typed in full whispers `Ulf Stone` while that player is
  online, since the longest name wins; whispering `Ulf` then takes picking
  the name from the list.
- The chat is resized with the mouse only and cannot be moved. A word wider
  than the chat is cut at the row's end wherever that falls, not at a
  syllable or after punctuation.
- The chat's filters are toggled with the mouse only. Names in scripts other
  than Latin, Greek, and Cyrillic match only in the case they are written in.
- A click made before the mouse has moved since the chat opened is dropped.
- The send target, the filters, the whisper partners, and the sent lines last
  until the game exits.
- The picker covers armor, weapons, torches, ammunition, and races, not spells
  or actors; `ItemKind` is where more kinds would go.
