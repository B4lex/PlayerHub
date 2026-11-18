# UI Overlay

## Scope

The plugin's own UI layer: [nameplates](#nameplates) over other players'
heads, a status panel, the [display name hint](#display-name-hint),
[notices](#notices), chat, the
settings window, the
[trade window and prompt](#trade-window), the [voice chat](#voice-chat)
indicators, and the reusable item picker with a
3D preview. `ui/` never changes the game; it reaches
the network only through the feature modules it presents, such as `ChatSync`,
and the configuration through `Config`. Its text, and the notices the
feature modules post, come from the string tables in `src/text/`
([Localization](#localization)).

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
  Fonts.h/.cpp                # system fonts with fallbacks, shipped themed faces and logo
  Theme.h/.cpp                # look of the interactive windows: accent, rounded borders
  MenuFrame.h/.cpp            # Skyrim's menu frame around a window, from its shipped shape
  Cursor.h/.cpp               # the overlay's own cursors in the frame's style, drawn at the mouse
  Window.h                    # interface every window implements
  WindowManager.h/.cpp        # registered windows, draw order, input focus
  InputCapture.h/.cpp         # input dispatch hook: focus keys, routing to ImGui
  InputLanguage.h/.cpp        # typed characters follow the game window thread's keyboard layout
  Layout.h                    # placement helpers (screen corners)
  hud/NameplateLayer          # other players' names projected above their heads
  hud/StatusPanel             # connection, ping, traffic, plugin time
  hud/DisplayNameHint         # where to enter a display name, while none is set
  hud/GameView                # whether the world is on screen, for the HUD layers
  hud/NoticeStack             # notices on the right edge, friends coming online
  hud/VoiceIndicator          # the microphone while the player is heard
  chat/ChatWindow             # open and closed chat, focus by its keys, the manager alone, the send target's channel kept shown
  chat/ChatHistory            # lines and their mentions, channel filters, unread marks, scrolling, the line menu
  chat/ChatInput              # target chip and its menu, text field, name list, notes
  chat/ChatSize               # the open chat's size, the drag that sets it, saving it
  chat/ChatPosition           # the open chat's corner, the drag that moves it, saving it
  chat/ChatPlayers            # the players online: the button on the filters' row, the list beside the chat
  chat/ChatStyle.h            # channel and failure colors, the chips' frame, the rule under the filters
  chat/ChatPlayerMenu         # a player's menu: heading, whisper, trade, friend, mute and vanish items or their undoing
  chat/ChatPlayerManager      # player manager: friends, muted, and vanished tabs, over the chat or alone
  chat/WorldPlayerMenu        # a player's menu at the crosshair, opened by activating its character
  trade/TradePrompt           # a trade invitation waiting for an answer, answered by its keys
  trade/TradeWindow           # the open trade: the layout of chat, panel, and preview column, the panel's offers as grids, own items, Accept and Cancel
  trade/TradeDock             # the trade layout's chat column, set by the trade window for the chat in the same frame
  trade/ItemCard              # the preview column's item card in the vanilla layout
  settings/SettingsWindow     # plugin settings: sidebar, section pages, footer
  settings/ConnectionControls # connection button, the line under it, one-line summary
  settings/DiscordLink        # the Discord symbol and its invitation, opening the invite in the browser
  widgets/Controls            # switch, accent and red buttons, icon buttons, search field, muted text, striped box
  widgets/RoleBadge           # a role's letter mark beside a name, and its whole badge
  widgets/ItemPicker          # item dropdown
  preview/ItemPreview         # 3D item model, spinning and turned by dragging, and cached thumbnails
  render/Gpu                  # frame target, runtime-compiled full-screen passes, image blend modes
  render/Backdrop             # the game's frame blurred and darkened behind a full-screen window
src/sync/chat/ChatSync        # send to a target and why it failed, the server's rejections, lines to display
src/sync/chat/ChatRoster      # connected players by name, completion, mentions in a line, whisper partners, the list's copy, the server's roles and their marks
src/sync/chat/ChatComposer    # send target, the mention at the caret, name list, notes, sent lines
src/sync/notices/Notices      # the notices queue any feature posts to
src/sync/nameplates/NameplateSync # per peer: head node, line of sight, fade, label, voice level; the pass's plates
src/sync/voicechat/VoiceChatSync # speaking levels, the local microphone's state, audio devices (voice-chat.md)
src/sync/filters/PlayerFilters # friends, muted and vanished players of the current server, publishing, friends coming online
src/sync/chat/PlayerActivation # the activated player, and a whisper picked in its menu
src/runtime/PluginTime        # plugin time per pass
src/text/Text                 # the languages, the game's language, text lookup, formatting, plural forms, field splitting
src/text/Gettext              # PO file parsing and Plural-Forms rules for the translations read at startup
locale/                       # gettext sources: the English template, one .po per shipped translation, deployed to PlayerHub/locale/
locale/generate.lua           # checks the translations and writes Text::Id and the English catalog
locale/xmake.lua              # the `locale` rule that runs it and compiles its output
src/text/Utf8Case             # capitals of Latin and Cyrillic letters in UTF-8
assets/fonts/                 # Sovngarde Light and Bold with their license, the logo font, deployed beside the DLL
assets/logo/                  # the logo's and the Discord symbol's SVGs and the script that makes their font
assets/icons/                 # SVGs of the icons in the logo font
assets/frame/                 # the menu frame's shape, deployed beside the DLL
assets/cursor/                # the font cursor's SVG, built into the logo font
```

Every screen is an independent `UI::Window`. `Overlay::Install` registers each
with `WindowManager`, and `Overlay::DrawFrame` draws them in registration order.
Windows never reference each other, and a window's header exposes only a
factory, such as `CreateChatWindow`, so its state stays in its `.cpp`. The
trade window places the chat through `trade/TradeDock`, which both include
([Chat in a trade](#chat-in-a-trade)).

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
  `backBufferHeight / 1080` (at least 0.5) times the [scale](#scale).
- When the frame has no vertices and no texture waits for the backend
  (`Overlay::HasWork`), the popup blur, `ImGui_ImplDX11_RenderDrawData`, and
  the render target swap are skipped. The ImGui frame itself is still built
  every frame.
- The overlay draws on the frame target (`Gpu::GetFrameTarget`): the engine's
  `kFRAMEBUFFER` render target view, where the menus draw, or the swap chain's
  view (`renderWindows[0].renderView`) when that is null. In vanilla both are
  the back buffer. Community Shaders' HDR output points `kFRAMEBUFFER` at a UI
  texture from the start of the menus' draw until Present, where it composites
  that texture over its HDR scene and overwrites the back buffer, so anything
  drawn on the back buffer itself would never show. Its frame generation (the
  D3D12 swap chain proxy) also points `kFRAMEBUFFER` at a UI buffer that it
  composites at Present, so the overlay is not interpolated with the scene.
- The DX11 backend does not save the output merger, so the overlay binds the
  frame target, draws, and restores the previous targets.
- `ImGuiConfigFlags_NoMouse` and `NoMouseCursorChange` are set unless a window
  takes the mouse, and `imgui.ini` is disabled.

### Scale

`[UI] Scale` ([Configuration](configuration.md#ui)) multiplies the overlay
scale (`Overlay::ApplyScale`), which sets `FontScaleMain` and rescales the
style from the unscaled one when the product changes, from the next frame
after the setting applies.

- Almost every length in the overlay is in font sizes, so the setting resizes
  the whole overlay with the text, cursor and menu frame included, rather than
  only the glyphs, which would need every layout redone. The cursor's speed
  ([Input focus](#input-focus)) stays on the display's scale alone.
- The [cursor](#cursor) is drawn as lines, or as glyphs rasterized at its
  size, rather than a scaled bitmap, so a fractional scale does not blur it.
- The trade window draws at a smaller font size where the display cannot hold
  its layout at the scale ([Window](#window)).
- A resizable window keeps its size across font size changes through
  `MenuFrame::BeginWindow` ([Menu frame](#menu-frame)).

### Backdrop

`Backdrop::GetBlur` gives the finished game frame, blurred, as a texture
with the UV of each display pixel (`Blur::ToUv`). `Backdrop::Draw` covers the
display behind every overlay window (ImGui's background draw list) with it,
darkened by `kTint`; the [trade window](#trade-window) draws it each frame.
`Backdrop::DrawBlur` puts it in a rounded rectangle and `BlurPopups` under
popups, for [translucent surfaces](#background-blur). The game keeps
running, so the blur is made again each frame from the frame's scene as the
overlay hook finds it, HUD and menus included unless they draw on a UI
layer, after `ItemPreview::Update` has put back what its capture changed, and
at most once a frame however many callers ask for it.

- It copies the scene (`Frame::scene`), reduces it to a quarter in each
  dimension, and blurs that with a separable Gaussian, so the blur scales with
  the resolution.
- `Gpu` (`ui/render/Gpu`) holds what the overlay's own GPU passes share: the
  frame target, its texture, and the scene (`GetFrame`), render targets,
  pixel shaders compiled at runtime by `D3DCompile` (`vs_4_0`, `ps_4_0`, from
  the `d3dcompiler_47.dll` the plugin already links), and `AddImage` with an
  opaque or premultiplied blend callback. `PassScope` draws full-screen
  triangles from `SV_VertexID` and on leaving restores every pipeline state it
  touched, since the engine tracks what it bound.
- A multisampled back buffer, a shader that does not compile, or textures or
  states that cannot be created turn the blur off for the session with one
  warning (`UI backdrop blur unavailable: <reason>`); the backdrop is then a
  plain dark fill, and translucent surfaces are drawn without a blur.
- The scene is the frame target's texture when that is the swap chain's back
  buffer. A frame target with another texture (`Frame::uiLayer`) holds the UI
  but not the scene. Community Shaders' frame generation leaves this frame's
  scene in the swap chain's buffer, so that buffer is blurred instead, without
  the HUD and menus. Its HDR output without frame generation writes the real
  swap chain's buffer only at Present, so the buffer holds no scene when the
  overlay draws. The two are told apart by the buffer's `miscFlags`: only a
  buffer shared with D3D12 (`D3D11_RESOURCE_MISC_SHARED` and
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`), the frame generation proxy's, is
  blurred; otherwise the backdrop is the plain fill without turning the blur
  off. One debug line per session (`UI backdrop: the frame target is a UI
  layer ...`) names the case.
- `Backdrop::Update`, before `ImGui::NewFrame`, frees the copy and the
  reduced textures when nothing asked for the blur in the last frame.

### Fonts

`Fonts::Load` reads from the system fonts folder (`FOLDERID_Fonts`, which is
`C:\windows\Fonts` in a Wine or Proton prefix). The first file found is the main
font, and each later one is merged and only supplies glyphs the earlier lack:

| Files | Adds |
| --- | --- |
| `segoeui.ttf`, `arial.ttf` | Latin, Greek, Cyrillic |
| `tahoma.ttf`, `micross.ttf` | Arabic, Hebrew, Thai |
| `Nirmala.ttc`, `nirmala.ttf` | Indic scripts |
| `msyh.ttc`, `msyh.ttf` | Simplified Chinese, and kana and Hangul in Proton's copy |
| `msjh.ttc` | Traditional Chinese |
| `YuGothM.ttc`, `msgothic.ttc` | Japanese |
| `malgun.ttf` | Korean |
| `seguisym.ttf` | Symbols |

Proton prefixes lack Segoe UI, Yu Gothic, and Segoe UI Symbol. Missing files
are skipped. Each file is memory-mapped read-only and given to the atlas with
`FontDataOwnedByAtlas = false`, which since 1.92 means the atlas reads it in
place for as long as it has the font, so the views are unmapped only after
`ImFontAtlas::Clear` drops the fonts ([Localization](#localization)), and only
pages with drawn glyphs are read: CJK fonts cost address space, not memory.
Dear ImGui 1.92 rasterizes glyphs on demand, so no glyph ranges are needed.
Without any file, ImGui's default font covers Latin only. The log lists the
fonts loaded.

Han characters take their shapes from the first Han font loaded, so
`GetSystemFiles` puts the text language's own first, by its code
(`GetHanOrder`): the Japanese fonts for `ja`, Microsoft JhengHei for
Traditional Chinese (`zh` with `_TW`, `_HK`, `_MO`, or `Hant`), and otherwise
Microsoft YaHei, then JhengHei, then the Japanese fonts. `Text::Initialize`
and the `[UI] Language` setting are therefore applied before `Fonts::Load`
([Localization](#localization)). Sovngarde covers the Latin and Cyrillic
letters of every shipped translation, so of those only Japanese and Chinese
text uses the system fonts.

#### Themed faces

The [themed](#theme) windows use Sovngarde 8.9 by mjorka, a serif after
Skyrim's title lettering, in two more atlas fonts: `Fonts::GetText` is
`Sovngarde Light.ttf` and `Fonts::GetHeading` is `Sovngarde Bold.ttf`. The
overlay's own font stays the atlas default, so the status panel keeps the
system font.

- The files are in `assets/fonts/`, unmodified from the
  [standalone release](https://mjorka.itch.io/sovngarde), with
  `Sovngarde OFL License.txt`, and are read from `PlayerHub/fonts/` under
  `Config::GetDirectory` ([Configuration](configuration.md#file));
  `xmake.lua` copies the folder to every install directory after a build.
- They are separate files rather than data in the DLL because Sovngarde is
  under the SIL Open Font License 1.1 and the plugin under GPL-3.0-or-later:
  beside the DLL the font stays a work of its own under its own license. The
  license text is deployed with the fonts and belongs in any release archive;
  the copyright notice is also in each font's name table. It has no reserved
  font name.
- Each face is a font of its own with the loaded system files merged after it,
  in the same order, so a character Sovngarde lacks comes from the system font
  at the same size. Sovngarde has ASCII, most of Latin-1, the Polish letters,
  and the Russian alphabet with Ukrainian `Є`, `І`, `Ї`; it lacks `Ґ`, the
  Turkish and most other Latin Extended-A letters, and `…`, `–`, `—`.
- Sovngarde's letters are small for its line height, so `kShippedGlyphScale`
  goes on its source as `ImFontConfig::ExtraSizeScale`, which enlarges glyphs
  and advances and leaves the line height: text is as wide as in the system
  font and layouts in font sizes hold.
- `Fonts::FindHeadingBounds` gives a glyph's outline bounds in the heading
  face per pixel of font size, at the scale ImGui's loader uses, since the
  atlas keeps only each baked glyph's whole-pixel box. It reads the mapped
  file through a private, static copy of `imstb_truetype.h` in
  `UI::Fonts::Stb` (outside it, the header's stand-in `stbrp_node` clashes
  with `imgui_internal.h`'s). It is empty while the face is not loaded and
  for a glyph the face lacks.
- A file that is missing or is not a font logs a warning and leaves its
  accessor null; `ImGui::PushFont` then keeps the current font, so the
  window shows in the system font.

#### Logo

`PlayerHub.ttf`, which `Fonts::GetLogo` returns, holds the PlayerHUB logo and
the overlay's icons as glyphs, so Dear ImGui rasterizes and caches them at
any size with the other glyphs and they stay sharp at every scale:

| Code point | Glyph |
| --- | --- |
| U+E000, U+E001 | The logo's letters and its underline bar |
| U+E002 | The Discord symbol of the [Discord link](#discord-link) |
| U+E003, U+E004 | The muted and vanished marks of the [player list](#player-list) |
| U+E005 to U+E007 | The [font cursor](#font-cursor) |
| U+E008 | The [voice chat](#voice-chat) microphone |
| U+E009, U+E00A | The close and reset icons of `Controls::IconButton` |
| U+E00B | The person on the player list's button |
| U+E00C | The [voice chat](#voice-chat) speaker |
| U+E00D | The location pin of the [player menu](#player-menu) |

- `assets/logo/generate.py`, run with `uv run --script`, builds the font with
  skia-pathops and fixed timestamps from `assets/logo/logo.svg`,
  `assets/logo/discord.svg`, `assets/cursor/cursor.svg`, and the icon SVGs
  that `ICONS` maps to code points in `assets/icons/`. The logo's em spans its
  height, so the font size in pixels is the logo's height; each icon is scaled
  so its height is the em and advances by its own width. An icon SVG holds
  only untransformed paths, filled or stroked with round caps and joins, in
  black or white; the script paints them in order, black adding to the glyph
  and white cutting it away, so the SVG in a browser looks like the glyph.
- The file is deployed and loaded with the [themed faces](#themed-faces), as
  a font of its own with no fallbacks and no `ExtraSizeScale`.
- `DrawBrand` heads the settings window's sidebar with the letters in the
  text color and the bar in the [accent](#accent-color), filling the width
  the connection summary gets; the font size is floored to whole pixels, so
  the logo never overflows that width.
- Without the file, the sidebar shows a text title in the heading face, and
  the log warns.

### Theme

The overlay's base style is `StyleColorsDark`, which the status panel uses.
`UI::Theme::Scope` gives every window and popup begun while it lives the look
of the plugin's interactive windows, by pushing ImGui style colors and
variables and popping them when it ends. `SettingsWindow` wraps its window in
one, which covers the lists and tooltips opened from it, the item picker's
included, and `ChatWindow` wraps the open chat and the recent lines in one
each. The settings window and the open chat have the [menu frame](#menu-frame)
in place of the style's background and border.

- Text is `Fonts::GetText`, pushed at the size in effect, so every size given
  in font sizes is unchanged. Headings push `Fonts::GetHeading` with their own
  size ([Themed faces](#themed-faces)).
- The accent is `[UI] AccentColor` ([Accent color](#accent-color)), given by
  `Theme::GetAccent` and, at an alpha, `Theme::Accent`. It marks what the
  player can act on or has selected: popup borders, the selected section and
  rows, a switch that is on, hovered and pressed controls, selected text, the
  tab shown, and the fill of the button a view leads to
  (`Controls::PrimaryButton`). Body text is never the accent, so warnings stay
  distinct.
- Text on the accent fill is `Theme::GetOnAccent`: `Theme::kOnState` or the
  body text color, whichever has the higher WCAG contrast with the accent.
- `Theme` works out the accent, its text color, the
  [frame color](#menu-frame), and the [backgrounds](#background-opacity) again
  only when `Config::GetRevision` changes, so a call costs one atomic load.
  These calls and `Scope` are for the overlay thread.
- A state is `Theme::kPositive`, `kWarning`, or `kNegative`: the connection
  indicator, the picker's warnings, values that are not applied, refusals, and
  save results. `kNegative` also fills the button that ends what is running
  (`Controls::DangerButton`). States do not follow the accent.
- Dear ImGui's resize grips are transparent, since a grip would show through
  the translucent frame backing; only the cursor shows where a window resizes.
- Style sizes are stored in font sizes, then floored to whole pixels and kept
  at 1 px or more, so they follow the overlay scale and a border never
  disappears at a small one.

`UI::Controls` holds the small widgets the themed windows share, each as high
as a frame: `Toggle`, a switch whose knob slides over frame time;
`PrimaryButton` and `DangerButton`; and `IconButton`, whose icons are
[logo](#logo) font glyphs; without the font the button keeps its hover fill
and shows no icon.

### Background opacity

Goal: one background opacity, set by the player, for every overlay surface
the player reads or acts in. Non-goal: an opacity per window.

- `[UI] BackgroundOpacity` ([Configuration](configuration.md#ui)) has a floor
  of 25, below which text over a bright scene is hard to read.
- `Theme::Scope` pushes `ImGuiCol_WindowBg` and `ImGuiCol_PopupBg` with the
  opacity as their alpha, so a change shows from the next frame. Everything
  drawn from those colors follows it: the backing of each
  [menu frame](#menu-frame) (`MenuFrame::BeginWindow` for the settings window,
  the trade panel, and the item card; `ChatWindow`'s `BeginFramed` for the open
  chat, the player list, and the player manager; `MenuFrame::BeginPrompt` for
  the trade prompt and the display name hint), [notices](#notices), and
  Dear ImGui's popups, lists, and tooltips, with the chat's name list and
  notes, which take the popup color. A popup or child over a window covers
  more than either alone.
- Not affected: the [status panel](#status-panel), which keeps its own
  lighter background (`SetNextWindowBgAlpha`) because it stays on screen over
  the game while the overlay is not in use; the closed chat's recent lines,
  which have only a text shadow; the trade cells' wells and thumbnail
  backgrounds, which must be opaque ([3D preview](#3d-preview)); and the trade
  [backdrop](#backdrop).

#### Background blur

Goal: a translucent surface shows the game blurred behind it rather than
sharp. Non-goals: a blur strength or switch of its own, and blurring other
overlay windows behind a surface.

- While `[UI] BackgroundOpacity` is below 100, every surface it applies to
  draws the [backdrop](#backdrop)'s blurred frame (`Backdrop::GetBlur`) under
  its background: the [menu frame](#menu-frame)'s silhouette, each
  [notice](#notices) (fading with it), and every Dear ImGui popup and
  tooltip. At 100 nothing is blurred and the frame is not copied.
- Popups and tooltips are windows Dear ImGui begins, so nothing in the
  plugin draws before their background. After `ImGui::Render`,
  `Backdrop::BlurPopups` finds each active popup or tooltip that is not a
  child and has a background, and inserts one of its own draw lists, holding
  the blurred frame in the window's rounded rectangle, into the draw data
  just before the window's list, adding its counts to the totals. The lists
  are kept in a pool registered with the context's shared draw data, which
  the overlay never destroys.
- The blur is the game's frame as it is before the overlay draws, so a popup
  or tooltip over an overlay window shows the game blurred where the window
  is, not the window. Child windows that take the popup color, such as the
  chat's name list and notes, are not blurred, since the window they are in
  is. The status panel is not blurred.
- While the full-screen backdrop is drawn in the same frame, the blur is
  tinted as it is (`Blur::tint`), so a surface over the trade window matches
  what is around it.
- Blurring costs a full-resolution copy of the back buffer and three
  quarter-resolution passes once per frame while any translucent surface is
  shown, notices included.

### Menu frame

The settings window and the open chat are framed as the game frames its
message boxes. `UI::MenuFrame` draws the frame from a file, so the game's art
is not part of the plugin.

- The art is shape 10 of `Interface/messagebox.swf` in the game's
  `Skyrim - Interface.bsa`. `menu-frame.shape` is FFDec's SVG export of the
  shape with its curves flattened: the outline of the silhouette and the line
  pieces, each polygon clockwise. The file is the source; nothing in the
  repository regenerates it.
- A point is measured from the two sides of the framed rectangle nearest to it
  across the sprite's scaling grid (`lt`, `rt`, `lb`, `rb`, then two lengths in
  the art's pixels). Drawing at any size is then one subtraction per
  coordinate: the corners keep their shape and the lines stretch, as the
  game's 9-slice does. The file also gives `inset`, how far the lines reach in
  from a side, and `corner`, how far the knots do.
- The file is `PlayerHub/frame/menu-frame.shape` under `Config::GetDirectory`,
  deployed by `xmake.lua`. It is a file rather than data in the DLL for the
  reason the [themed faces](#themed-faces) are: the art is Bethesda's and the
  plugin is under GPL-3.0-or-later.
- `MenuFrame::Load` runs once when the overlay initializes. It accepts
  `frame 1` as the first line, then `inset`, `corner`, and polygons as
  `backing N` or `line N` followed by N points; lengths must be finite and
  within 0 to 4096, a polygon has at least 3 points, and a file at most 8192.
  A file that is missing or malformed logs a warning with the reason and leaves
  the shape empty.
- `MenuFrame::Draw(line, backing)` fills the current window's rectangle: the
  silhouette in the backing color, then the pieces in the line color, each with
  `ImDrawList::PathFillConcave`. With a translucent backing, the
  [blurred frame](#background-blur) goes under the silhouette first: the
  backing's triangles again, textured with the blur at each vertex's
  anti-aliased coverage, in an earlier draw list channel. The window is begun
  with `ImGuiWindowFlags_NoBackground`, and the clip rectangle is the whole
  window, since a window's own leaves out part of its padding.
- Triangulating the concave pieces is most of what a frame costs (about 30 µs),
  so `Draw` keeps the triangles of the 4 frames it drew last, keyed by
  everything that shapes them: the window's size, the scale, both colors, the
  font atlas's white pixel, and the draw list's anti-aliasing flags and
  fringe. A window drawn again with the same key copies them, moved to its
  corner; a drag that resizes it triangulates in every frame. A frame during
  which the draw list started its vertices over is not kept. Nothing is
  allocated once the draw list and the kept frames have grown.
- The line color must be opaque: the anti-aliased fill overlaps some
  triangles of these fine pieces, which a translucent color would show.
- The art's scale follows the font size times `[UI] BorderThickness`
  ([Configuration](configuration.md#ui)), read each frame; 100 is the size the
  game draws the frame at, at any resolution. Lines, knots, and inset scale
  together.
- `MenuFrame::BeginWindow` begins the settings and trade windows the same way:
  centered and sized in font units the first time they are shown
  (`ImGuiCond_Once`; a fixed window is placed every frame), focused on request,
  padded by the frame's inset, and with the frame drawn behind the content. A
  resizable window keeps its size and position in its ImGui state storage each
  frame it is drawn; when the font size differs from the kept one, including
  on the first frame after reopening, it is resized by their ratio around its
  kept center and clamped to the display. The kept rect, not the current one,
  is scaled because `Begin` has by then grown the window to the new minimum
  size.
- Dear ImGui resizes a window from its edges and its lower corners only, so
  `BeginWindow` adds grips on a resizable window's upper corners
  (`GrabUpperCorners`), with the hit areas of Dear ImGui's own corner grips,
  under a clip rectangle widened to the display. A drag on one keeps the
  opposite lower corner in place. `FollowUpperCorner` gives the window the
  dragged rect before `Begin`, within the minimum size and the display, so it
  moves in the same frame as the cursor.
- Dear ImGui moves a movable window by a drag on its empty space but sets no
  cursor for it, so `MenuFrame::ShowMoveCursor`, called after `Begin` by
  `BeginWindow` and by the chat's `BeginFramed`, sets `ResizeAll` while Dear
  ImGui's `MovingWindow` is the current window and the drag has passed Dear
  ImGui's drag threshold, as the [chat's move](#position) does. Of the
  chat's windows only the [player manager](#player-manager) can be moved
  this way.
- `MenuFrame::BeginPrompt` begins the [trade prompt](#prompt) and the
  [display name hint](#display-name-hint) the same way: sized to the content,
  without inputs or focus, centered near the top under the compass, and
  framed over the window background. On the frame it reappears it lays out but
  is not rendered (`ImGuiWindow::Hidden`), since ImGui sizes a reappearing
  auto-resized window from the contents it last had and hides only new windows
  and popups for that frame.
- `MenuFrame::GetMetrics` gives a window `inset` and `corner` in pixels to lay
  its content out by, so content stays clear of the knots.
- The line color is `Theme::GetFrameColor`, which follows `[UI] BorderColor`
  ([Configuration](configuration.md#ui)) each frame: `Vanilla` is the game's
  frame color as it looks over the dark backing, and `Accent` the
  [accent](#theme) as it looks over the theme's surface, so it follows
  `[UI] AccentColor` too. The backing is the window's own background color, at
  the [background opacity](#background-opacity), and a window's popups keep
  their accent border with either.
- Without a shape, `Draw` fills the style's rounded rectangle and, where the
  style has a window border, outlines it in the line color; the metrics are
  then the style's border width and rounding, so a window lays itself out the
  same way with or without the shape.

### Cursor

The overlay draws its own cursors in place of Dear ImGui's software cursor.
The normal one is the [font cursor](#font-cursor); the others are in the
style of the [menu frame](#menu-frame), which `UI::Cursor` builds in code.

- `Cursor::Draw` runs after the windows and before `ImGui::Render`, while
  `InputCapture::ShowsCursor` holds, and draws the shape for
  `ImGui::GetMouseCursor` at `io.MousePos` on the foreground draw list, so it
  is above every window. `io.MouseDrawCursor` stays off.
- Shapes: the pointer for `Arrow`, and also for `Hand`, `NotAllowed`, `Wait`,
  and `Progress`; an I-beam for `TextInput`; a resize shape for each `Resize`
  direction and a move shape for `ResizeAll`. `None` draws nothing.
- The pointer is the [font cursor](#font-cursor) while `Fonts::GetLogo` has
  its font, and the plugin's drawn pointer without it.
- A drawn shape is layers of strands, open or closed polylines, each with a
  backing and a border, so the strands of a layer join cleanly and a later
  layer's border cuts the lines it crosses. Lengths are in `kUnitFonts`, so
  the shapes follow the overlay's scale.
- Dear ImGui keeps a tooltip out of a box from the mouse sized by
  `ImGuiStyle::MouseCursorScale`, which is meant for its own cursor.
  `Cursor::PlaceTooltips` sets the scale each frame before `ImGui::NewFrame`
  so the box is the normal cursor's extent plus `kTooltipGapFonts`, so a
  tooltip never covers the cursor. `ScaleAllSizes` would otherwise truncate
  the scale to a whole number.
- The line color is `Theme::GetFrameColor`, so the drawn cursors follow
  `[UI] BorderColor`. The backing is opaque, unlike the frame's, so they stay
  visible at any background opacity.

#### Font cursor

- `assets/cursor/cursor.svg` is the plugin's own art, in the style of the
  game's menu cursor. It holds one even-odd path per fill: `outline`, `band`,
  and `centre`. The file is the source; nothing in the repository regenerates
  it.
- `assets/logo/generate.py` ([Logo](#logo)) resolves the even-odd paths into
  nonzero contours, scales the shape so its height is the font's em, stretched
  by `CURSOR_PLACEMENT`, the scale `Interface/cursormenu.swf` places the
  game's cursor at (`mc_Cursor`'s matrix), and adds the three fills as
  U+E005 to U+E007. The tip is at the glyphs' top left, so the font size in
  pixels is the cursor's height and the text position its tip.
- `Cursor::Draw` draws the three glyphs at `io.MousePos` in `kVanillaOutline`,
  `kVanillaBand`, and `kVanillaCentre`, the centre opaque and in its own
  glyph so nothing shows through it. `kVanillaHeightFonts` sets the height in
  font sizes, smaller than the game shows it
  ([Engine assumptions](#engine-assumptions)).

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
- `WindowManager::FocusByKey` tells the window which of its focus keys gives
  it focus (`Window::OnFocusKey`, on the input thread), for a window with
  several, such as the [trade prompt](#trade-window) and the chat's
  `OpenChat` and `ManagePlayers`. It calls it before publishing the focus,
  so the overlay thread never draws the window focused without the key.
- The first `CharEvent` while the key that gave focus is still held is
  dropped, so a focus key that types, such as `T` bound to `OpenChat`, does
  not start the text it opened the field for.
- A window that opens without a key, the [player menu in the
  world](#player-menu-in-the-world) and the chat it opens to whisper, takes
  focus with `WindowManager::Focus` from the overlay thread, only while no
  window holds it. The [trade window](#trade-window), which the player must
  see when the other player accepts, takes it from any window with
  `WindowManager::TakeFocus`. The next dispatch releases either if gameplay
  is not the active context.
- A focused window that returns true from `Window::LendsFocus` (read on the
  input thread) lends focus to a window whose `Window::IsBorrowKey` takes the
  pressed key: the trade window, while none of its fields or popups has the
  keyboard, lends it to the chat's `OpenChat` key
  ([Chat in a trade](#chat-in-a-trade)). `WindowManager` keeps the
  lender, under `s_focusLock` with the focused window, and
  `ReleaseFocus(*this)` by the borrower gives focus back to it rather than
  to the game; the lender releasing drops it, and the borrower keeps focus.
  `TakeFocus` and the release of every window on a menu drop the lender as
  well. `WindowManager::Borrow` lends the same way without a key, for a
  click on the borrower: the chat docked in a trade.
  `WindowManager::HoldsFocus` tells a window whether it is focused or lent
  as of the call, since `WindowInput::focused` is the start of the frame's
  draw. A borrower never hides the idle cursor
  (`FocusedWindowHidesIdleCursor`), so the lender's cursor stays shown where
  it is.
- A window gives focus back with `WindowManager::ReleaseFocus(*this)`, or names
  release keys with `Window::IsReleaseKey`: `InputCapture` releases the focused
  window when such a key is pressed, and neither the window nor the game sees
  that press. A release key that edits text, an ImGui key such as Delete or
  Home or a key that types a character on common layouts (letters, digits,
  punctuation, Space, the numpad), goes to a text field instead while one is
  active (`io.WantTextInput` as of the overlay's latest frame), so it still
  edits.
- Every keyboard press and release the dispatch routes is also recorded in
  `KeyState` ([Sync pipeline](sync-pipeline.md#components)), whoever owns the
  key, for [voice chat](voice-chat.md#activation)'s microphone key; a press
  that edits an active text field is not, and a release always is. Nothing is
  taken from the game or a window by it.
- While the focused window returns true from `Window::CapturesKeys`, each
  keyboard key pressed goes to its `Window::OnCapturedKey`, on the input
  thread, ahead of release keys, and neither ImGui nor the game sees the
  press. The [settings window](#key-bindings) so binds its own release key
  and Escape.
- `InputCapture` replaces the `call` at `+0x7B` in
  `BSInputDeviceManager::PollInputDevices` (IDs 67315/68617), which is
  `BSTEventSource<InputEvent*>::SendEvent(this, &head)` on 1.7.104 and feeds the
  menu and player-control sinks. The engine calls `ClearInputQueue` right after,
  and `PushOntoInputQueue` resets each event's `next`, so the thunk relinks the
  list to drop the events the game must not see. It refuses to patch anything
  but a `call`.
- Other plugins with ImGui menus, such as Open Animation Replacer, detour the
  same `call`, and whichever detours last runs first. A plugin
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
- Focus keys do nothing while another plugin's menu holds input. Community
  Shaders and SKSE Menu Framework hold it by dispatching an empty list in
  place of the events from their detour of the same `call`. `InputCapture`
  prepends a `BSTEventSink<InputEvent*>` (`DeliverySink`) to the dispatch's
  source on its first call, ahead of any sink that could stop the dispatch.
  After each routed list led by a keyboard or mouse event, the sink records
  whether the game received an empty list (`s_inputHeldElsewhere`). Lists led
  by a gamepad event say nothing, since Community Shaders lets those through,
  and neither does a dispatch the sink does not see. While input is held,
  a focus key goes to the game's list like any other key, so the other menu
  receives it. A debug line is logged each time the state changes.
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
  buttons through ImGui: `InputCapture` clears `NoMouse`,
  shows the overlay's [cursor](#cursor) (`InputCapture::ShowsCursor`), and moves an absolute cursor by DirectInput's relative
  counts, scaled by `backBufferHeight / 1080` and clamped to the back buffer. The
  cursor starts at `Window::GetCursorStart`, or centered without one, whenever
  such a window takes focus, and its position is queued after the Win32
  backend's, which follows the hidden system cursor. Left,
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
  `CharEvent`s.

### Input language

Goal: switching the Windows input language while the game has focus changes
the layout typed into a window's text field at once, as it does elsewhere in
Windows.

- Typed text comes from Skyrim's `CharEvent`s, which
  `BSWin32KeyboardDevice::Poll` (ID 68782, SE 67472) builds with
  `MapVirtualKeyA` and `ToUnicode`, so they follow the layout of the thread
  that polls. Windows switches the language only for the thread that owns the
  game window. The poll runs on that thread while the game has input, but on
  other threads while a window holds focus, and those threads keep the
  layout they started with.
- `InputLanguage::Install`, called from `Overlay::Install`, replaces both
  `call [rip+disp]` sites (+0xEC, and +0x2CB on 1.7.104 or +0x20D on SE, the
  only calls to either import in each executable) with `write_call<6>`, after
  checking that each still reads the user32 import. The replacements call
  `MapVirtualKeyExA` and `ToUnicodeEx` with `GetKeyboardLayout` of the game
  window's thread, found once from the swap chain's window; until that window
  exists they use the calling thread's layout, as the game did. The game's
  menus follow the language the same way.
- When the layout used changes, including the first key, the replacement
  logs it at debug with the polling thread, that thread's own layout, and
  the character produced.
- The plugin reads the window thread's layout and never activates one:
  `ActivateKeyboardLayout(…, KLF_SETFORPROCESS)` from the input dispatch
  hangs the game on Windows.

## Status panel

`StatusPanel` sits in the top-right corner (`kCorner`) on its own lighter
background and takes no input. It is drawn only during a game
(`GameSession::IsInGame`) and while `[Advanced] StatusPanel` is on; a change
applies on the next frame.

- Not connected, whatever the reason, it shows the disconnected state.
- Connected, it shows the ping, without a value before the first sample or
  after a timeout, colored by `kGoodLatencyMilliseconds` (80) and
  `kFairLatencyMilliseconds` (150), then the received and sent traffic in
  KB/s ([Network](network.md#traffic)), without a value before the first
  window.
- Always last: the [plugin time](#plugin-time) in ms, without a value before
  the first window.

The panel is a debugging readout, not player guidance. It does not say why a
connection is down or that a display name is missing: those need a sentence
and an action in the settings window, so the messages are on its
`[Connection]` page ([Connection controls](#connection-controls)), and the
[display name hint](#display-name-hint) leads there.

### Plugin time

`PluginTime::Scope` wraps the plugin's work in the main-loop hook, the
renderer hook, and input routing, adding its `std::chrono::steady_clock`
duration to a shared atomic total, since the renderer hook may run on another
thread. The main-loop hook calls `PluginTime::EndFrame` once per pass after its
scope closes; time before the first pass is dropped. Once at least
`kAverageWindowSeconds` (100 ms) of wall-clock time has passed, the total
divided by the passes in the window becomes the published average and a new
window starts, so the number changes about ten times a second. It is always
compiled in and costs two clock reads per hook call. It leaves out the engine
hooks that run per actor on animation and AI threads. What the plugin costs
and how to profile it in detail is in [Performance](performance.md).

## Display name hint

Goal: a player without a display name, who therefore cannot connect
([Network](network.md#sessions)), learns where to set one. Non-goals: a
dismiss control or a timeout: without a name the player cannot play online
at all, so the hint stays until one is applied.

- `DisplayNameHint`, registered after the trade prompt, draws while a game
  is loaded (`GameSession::IsInGame`), the
  [vanilla intro](network.md#vanilla-intro-hold) does not hold the connection
  (`VanillaIntro::IsPlaying`), the applied `[Connection] DisplayName`
  is empty, no overlay window holds focus, and `UI::IsGameViewShown` holds.
  It therefore shows exactly when `OpenSettings` would open the settings
  window ([Input focus](#input-focus)), and hides while that window is open.
  A name applied in the settings window removes it on the next frame.
- It is the trade prompt's panel (`MenuFrame::BeginPrompt`,
  [Prompt](#prompt)): framed, at the top center under the compass, without
  inputs or focus. Both share the spot because no trade can run without a
  display name.
- One line tells the player to press the `[Controls] OpenSettings` key,
  shown by its label (`Config::FormatKeyLabel`) in the accent, and to enter
  a display name on the `[Connection]` page, its title also in the accent. The
  settings window then opens on that page with the keyboard in the field
  ([Settings window](#settings-window)).
- `UI::IsGameViewShown` (`ui/hud/GameView`) is true while menus are shown
  (`UI::IsShowingMenus`), the game is not paused (`UI::GameIsPaused`), and
  neither the dialogue nor the race menu is open. The
  [nameplates](#nameplates) use it too.

## Nameplates

Goal: show each other player's display name above their character's head
while the character is within `[UI] NameplateDistance` of the player and in
sight, so players can tell who is who. Friends' names are in the accent, and
a [role](#roles)'s mark stands before its holder's name.
Non-goals: a level, health bar, or other detail on the plate, plates for
players without a character in the local world, plates through walls, and a
plate for the player's own character.

- `NameplateSync` (`sync/nameplates/`) runs on the game thread at the end of
  each pass, after every peer is synced
  ([Sync pipeline](sync-pipeline.md#frame-sequence)). `Clear` at the pass's
  start empties the plates, so a pass that returns early leaves none.
  Its per-peer `RemoteState` is a member of `PeerState`.
- A peer has a plate only while its remote actor is `ready` and its third
  person 3D has a head node: the node named by the race's body part data
  for `kHead` (`BGSBodyPart::nodeName`, `NPC Head [Head]` on vanilla
  races), found with `GetObjectByName` once per 3D root. The root and the
  head are held as `NiPointer`s, so a new 3D at the same address cannot be
  mistaken for the old one; both are dropped when the actor is gone. A
  mounted player's plate follows the rider's head.
- Distance is from the player's position to the actor's, compared with
  `NameplateDistance` in game units. Within it, `PlayerCharacter::HasLineOfSight`
  decides sight: for the player it tests the actor against the camera's
  frustum and casts a ray only for one inside it. An in-frustum result is
  kept for 0.2 s; an actor outside the frustum keeps its last result and is
  tested again after 0.05 s, so a plate appears soon after the camera turns
  to it. At most 8 rays are cast per pass; a peer over that budget is
  tested in a later pass. Out of range a peer's result is cleared, so it
  comes back only after a fresh test.
- Each plate fades in and out over 0.2 s of clamped frame time as its result
  changes. Its label is the peer's roster name with `PlayerLabel::Format`,
  or the appearance's name for a player the roster lacks, and it is a
  friend's by `PlayerFilters::IsFriend` on the roster's session. Its
  [role](#roles)'s mark is `ChatRoster::FindRoleMark` of the roster entry's
  role. All three are
  read again only when the roster's revision (`ChatRoster::GetRevision`),
  which a change to the roles advances too, or
  the filters' revision changes.
- `NameplateSync::GetPlates` gives the pass's plates sorted farthest first,
  each with the head node, label, role mark, friend flag, fade, and distance as a
  fraction of the range. The node and label are owned by the peer's state
  until the next pass. Off the main-loop thread it returns none: the overlay
  is expected to draw on that thread ([Item picker](#3d-preview) relies on
  the same), and the layer reads node transforms.
- `NameplateLayer`, registered before every other window, draws them on Dear
  ImGui's background draw list, under every window and under the
  [backdrop](#backdrop), which hides them behind the trade window. It draws
  nothing outside a game, with `[UI] Nameplates` off, or while
  `UI::IsGameViewShown` is false: the game is paused, menus are hidden, or
  the dialogue or race menu is open ([Display name hint](#display-name-hint)).
- A plate's anchor is `kHeadClearance` above the head node, times the node's
  world scale, projected each frame with `Main::WorldRootCamera`, so it
  follows the head as rendered. `NiCamera::WorldPtToScreenPt3` divides by the
  depth's absolute value, so the layer computes the depth from the
  `worldToCam` matrix's fourth row first and skips a point at or behind the
  camera. The name stands on the anchor in the heading face
  (`Fonts::GetHeading`, or the overlay's font without it), sized from
  `kNearFontScale` near the player to `kFarFontScale` at the range, in the
  accent for a friend, over an outline. A [role](#roles)'s mark stands before
  the name, which keeps its own color, and the [voice chat](#voice-chat)
  speaker before both; they are centered on the anchor as one. The plate fades
  out over the last `kDistanceFadeFraction` of the range; a plate wholly off
  screen is skipped.
- Per pass the cost is one distance per peer with a character, the budgeted
  rays, and one roster and filter revision read; nothing allocates once the
  plates vector has grown. Drawing costs five text draws per plate, and two
  rectangles and a glyph for a mark.

## Notices

Goal: tell the player what happened outside the chat's input, such as a
friend connecting or a trade ending, briefly and without filling the chat.
Non-goals: a log of past notices, notices in the chat's lines, and an
action on a notice. What answers the chat's own input is a
[note](#notes) instead.

- `Notices` (`sync/notices/`) is a queue of sentences any thread posts to
  with `Notices::Post`. It keeps
  the 32 newest, so it stays small where no overlay drains it, as in VR. Its
  posters are [trade](trade.md#notices), [mute and vanish](#mute-and-vanish),
  and `ChatWindow` for a server's rejection while the chat is closed
  ([Sending](#sending)).
- `NoticeStack` (`ui/hud/`) drains it every frame, with
  `PlayerFilters::CollectArrivedFriends` for friends coming online
  ([Friends](#friends)), and keeps at most 4: a fifth drops the oldest.
- `NoticeStack` also announces each connection the server welcomes: when
  `Network::GetConnectionGeneration` changes and `Network::IsConnected`
  still holds, it adds a notice that the player is connected, ahead of
  that frame's other notices. Automatic reconnects are announced too, and the
  notice does not name the server. A generation that ended before the stack
  saw it is not announced.
- `NoticeStack` also announces a `kAddressFull`
  ([Network](network.md#admission)) or `kKicked`
  ([Network](network.md#kicks-and-bans)) refusal when
  `Network::GetConnectionRefusal` starts returning it (`GetRefusalNotice`),
  since these can end a connection the player did not start, with the
  settings window closed. Like every refusal they wait for Connect
  ([Connection controls](#connection-controls)), and the kick notice names
  no reason. Other refusals, `kBanned` included, show only in the connection
  controls. A refusal cleared and set again between two frames the stack
  draws is announced once.
- Every notice is its own toast, a repeat too. Notices, [notes](#notes), and
  a trade row's reason share their texts: a chat rejection is a note or a
  notice depending on whether the chat is open, and a reason is both the item
  card's warning and part of a notice.
- A notice shows for `kReadingDelaySeconds` plus its length at
  `kReadingCharactersPerSecond`, between `kMinimumShowSeconds` and
  `kMaximumShowSeconds`, and fades over its last `kFadeSeconds`, timed by
  `ImGui::GetTime`. Notices keep their time while none is drawn, outside a
  game (`GameSession::IsInGame`); in a game they show in menus too.
- They stack on the right edge from `kTopFraction` of the screen height, the
  oldest on top, like the chat from the left. Each is a box with the theme's
  window background ([background opacity](#background-opacity)) over the
  [blurred frame](#background-blur), fading with the notice, its text in the
  theme's face, and an accent strip on its left edge; notices have no kinds.
  The strip and the surface are clipped from one rounded box
  (`Controls::StripedBox`), since a separate strip rect would have its
  rounding clamped to its narrow width.
- They are drawn in a `Theme::Scope` on Dear ImGui's foreground draw list,
  over every window, so they take no input and stay visible while a window
  is open, the [trade window](#trade-window) included, so a trade's notices,
  such as an item the other game cannot receive, are seen while trading. A
  notice can therefore cover part of a window under it. `Cursor::Draw` adds
  the cursor to the same list after every window has drawn, so the cursor
  stays over the notices.

## Chat

Players talk in plain text on three channels. Chat is presentation only. The
wire, the roster, routing, and server rules are in [Network](network.md#chat).

Goal: talk to everyone, to players nearby, or to one player; always see where
the next line goes and change it with a key or a click; keep whispering the
same player without naming it again; see who is online and whisper one from
a list; [mention](#mentions) a player by a few letters of its name, and see
at a glance a line that mentions oneself; tell the channels apart at a
glance, and turn each off and on; see as many lines as the player wants, by
dragging the chat larger; place the chat elsewhere on the screen.
Non-goals: chat commands (text starting with `/` is sent as written; Tab, the
chip, and the player list cover what they did), a whisper by an `@` prefix
(`@` mentions instead), chat history for late joiners, and a saved log.

| Channel | Reaches |
| --- | --- |
| Nearby | placed players within `chat.nearby_radius` (8192) in the same worldspace, or in the same interior |
| Global | every connected player |
| Whisper | one player |

Each channel has its own color in `ChatStyle`. `ui/chat/` draws and forwards
keys; what they mean is in `sync/chat/`, which has no Dear ImGui and no
engine access.

### Send target

`ChatSync::Composer` holds the target (`ChatSync::ChatTarget`): a channel, and
for a whisper a player by slot and name. It starts as Nearby, survives closing
the chat, and is not saved. `ChatInput` shows it as a chip in the channel's
color left of the [text field](#text-field), level with its last line, naming
the channel or, for a whisper, the recipient.

| Input | Effect |
| --- | --- |
| Tab, Shift+Tab | The next or previous of Global, Nearby, and the 3 latest whisper partners, most recent first. A whisper target that is not among them yet stands after Nearby |
| A click on the chip | A menu of Global, Nearby, and the 5 latest partners |
| A click on a sender's name, or the whisper item in a line's menu | A whisper to that player |

- Each row of the chip's menu has a tooltip saying who its target reaches.
  `DrawRow` reports the row's `ImGuiHoveredFlags_ForTooltip` hover from its
  selectable, so the whole row shows it.
- `ChatRoster` keeps the whisper partners, at most 8, moved to the front by
  every whisper sent or received except one to oneself, and compared by name
  as well as slot, since a player who connects again has another slot.

### Text field

`ChatInput::Draw` draws a word-wrapped multi-line field
(`ImGui::InputTextEx` with `ImGuiInputTextFlags_Multiline` and
`ImGuiInputTextFlags_WordWrap`) that holds one paragraph: the text never
contains a line break.

- It grows upward one line at a time as the text wraps, up to
  `kMaximumFieldLines` (7), then scrolls inside to the caret. The chat window
  keeps its height: `ChatWindow` sizes the window for a one-line field and
  gives the history `ChatInput::GetFieldHeight`'s height less.
- `ChatInput::Measure` counts the wrapped lines and the caret's line as Dear
  ImGui breaks them (`ImFontCalcWordWrapPositionEx` with
  `ImDrawTextFlags_WrapKeepBlanks` at the field state's `WrapWidth`), in the
  field's callback every frame and wherever the text changes outside it (a
  send, Escape, a name picked with the mouse). The height follows one frame
  after an edit inside the field.
- The text holds at most `Multiplayer::kMaximumChatTextCharacters` (512 code
  points). `ChatInput::FilterCharacter` takes the room left once per frame,
  counting a selection the input will replace as free, and drops every
  character past it, typed or pasted.
- A pasted line break becomes a space. Shift+Enter and Ctrl+Enter, which a
  multi-line field turns into a line break, send as Enter does.
- Up and Down move the caret between wrapped lines; on the first line Up,
  and on the last line Down, [recall](#sending) sent lines instead. The field
  moves its caret before its callback runs, so the callback decides from the
  caret line measured the frame before.
- While the field is not active, `ChatInput::Draw` clears the ID of Dear
  ImGui's retained edit state (`ImGuiInputTextState::ID`) when it still
  belongs to the field, so each activation starts a fresh state. Dear ImGui
  (1.92.9 through at least 1.93.1) lays out an inactive word-wrapped field
  whose wrap width changed with the caret of that old state, measured over the
  text the field now shows; a caret past that text's end loops forever in
  `ImFontCalcTextSizeEx`. The docked and the open chat draw the same field
  (`##Chat/##ChatInput`) at different widths and font sizes, so without this
  the game hangs when a trade ends, or the chat opens over one, after a long
  line was sent. Undo history does not carry across activations.

### Name list

While the caret ends a [mention](#mentions) being typed, `@` and the name
characters (`Multiplayer::IsDisplayNameCharacter`) before it, a list above
the field offers the connected players whose names start with what is typed
after the `@`, at most 8, as soon as the `@` is typed.

- `Composer::Update` finds that mention from the text and the caret, which
  the field's callback passes every frame. It looks again only after an
  edit or a caret move, and asks `ChatRoster` again only when the mention
  changes. A line recalled with Up or Down lists nothing until the caret
  moves or the text changes, so Up and Down keep recalling.
- `ChatRoster` copies the roster from the transport when its revision changes
  ([Network](network.md#client)) and sorts it by folded name. `Complete`
  returns the player named exactly as typed, then whisper partners, then
  players within Nearby chat ([Network](network.md#nearby-players),
  `Network::CopyNearbyPlayers`), then the rest by name, without the local
  player. The channel does not narrow it.
- Names are folded by a table for ASCII, Latin-1, Latin Extended-A, Greek,
  and Cyrillic, and compared byte for byte otherwise. Both sides of every
  comparison are folded the same way, so the server's lower-casing is not
  needed.
- The list shows only while it has a name to offer, so with nobody matching,
  or a name typed in full that no other name continues, Enter sends.
- Up and Down move the selection, and the field's caret stays; Tab, Enter, or
  a click accept it (`Composer::AcceptSuggestion`). That replaces the whole
  mention with `@` and the name as the player has it, then a space unless one
  follows already, and puts the caret after it. Enter and a click leave the
  field, so the caret is placed when the field takes the keyboard again
  (`Composer::TakePlacedCaret`). A name that would take the text past its
  [limit](#text-field) is not written and the list hides.
- Escape hides the list until the text changes.

### Sending

- Enter sends the text to the target and closes the chat; Enter in the empty
  field only closes. With `[UI] CloseChatOnSend = false`
  ([Configuration](configuration.md#ui)), Enter sends and the chat stays
  open with the field emptied, the target kept, and the keyboard back in the
  field, and Enter in an empty field does nothing; Escape then closes it. A
  send that keeps the chat open becomes what Escape returns to, so Escape
  never brings back a line already sent. `ChatInput` reads the setting only on
  such an Enter.
- `ChatSync::Send` trims the text, validates it (`protocol/ChatMessage.h`,
  shared with the parser), and `Network::PublishChatMessage` sends `ChatSend`
  with the channel, the recipient's slot for a whisper, and a request ID. A
  whisper resolves its player with `ChatRoster::Resolve`: the slot while that
  connection lasts, otherwise the connection that has the name now.
- When nothing was handed to the server, `ChatSync::Send` returns why
  (recipient offline, not connected, or characters that cannot be sent), and
  the chat stays open with the text, the target, and a failure [note](#notes).
- A rejecting `ChatResult` runs its callback on the transport thread, which
  queues each server rejection (`Multiplayer::ChatRejection`) as a sentence
  (`ChatSync::DescribeRejection`). `ChatWindow` takes them every frame: a
  failure note while the chat is open, otherwise a [notice](#notices).
- Up on the field's first line and Down on its last, while no list shows,
  step through the last 50 lines sent in this game session, consecutive
  repeats kept once. A recalled line restores its target. Down past the
  newest returns the text and target that were being written.

### Notes

What the chat answers to the player's own input is a note above the field,
never a line, so the history holds only what players said.

- Every note is a failure. `Composer` keeps them as text and replaces them
  on every send. Any edit, a recalled line, a new target, and opening the
  chat clear them; the field's text is kept.
- `ChatInput::DrawNotes` draws them where the name list would be, which hides
  them while it shows, as a child window with the popup's background in
  `ChatStyle::kFailureColor`. The panel takes no input.

### Lines and filters

- `ChatSync::DrainLines` returns received messages. A line carries its label
  and the other player: the sender, or the recipient of the local player's
  own whisper, whose label marks it as sent to the recipient. Senders are
  shown as a player label
  ([Appearance and equipment](appearance-and-equipment.md#player-label)).
- `ChatHistory` keeps the latest 100 lines of each channel, each stamped with
  `ImGui::GetTime()` when it arrived, so a busy channel only pushes out its
  own lines.
- A row above the lines has a filter chip for each channel
  (`ChatStyle::DrawChipFrame`). Each turns its channel's lines off or on, in
  the open chat and among the closed chat's lines, independently of the
  others. All are on at first. `ChatHistory` keeps collecting the lines of a
  channel that is off, so turning it on shows what arrived meanwhile. A rule
  separates the filters from the lines.
- A filter does not change the send target. A channel that is off turns on
  when the target changes to it, by any input (`ChatWindow`), and when a line
  the local player sent arrives in it (`ChatHistory::Collect`), so where the
  next line goes and what the player said stay in view. It can be turned off
  again and then stays off until either happens.
- A filter that is off shows an unread mark while lines of its channel
  arrived that the open chat has not shown. Each filter has a tooltip
  (`ImGui::SetItemTooltip`) saying which lines its channel holds and what a
  click does.
- A line's label, up to and with its colon, takes the channel's color and the
  heading face ([Themed faces](#themed-faces)); the text is in the text face.
  Under the cursor a label is underlined; a right click on a line with another
  player, its sender or the recipient of one's own whisper, opens its
  [player menu](#player-menu).
- The label's player's [role](#roles) mark stands in the label right before
  the name, so after a whisper's direction where the language puts that
  first: `ChatSync` gives the name's offset in the label, where the label's
  format has its field. `DrawLabel` draws the label's parts and the mark as
  one group, which is what the cursor underlines and what the line's text
  wraps beside. The mark is that of the role the player had when the line
  arrived (`ChatHistory::Line::role`), looked up again whenever the roster's
  revision changes.
- A line wider than the chat wraps at words. Its text starts beside its label
  and continues from the chat's left edge under the label, so a long name does
  not narrow every row (`DrawAfterLabel`), in the open chat and among the
  closed chat's lines.
- `FindRowEnd` ends each row where Dear ImGui's word wrap would
  (`ImFont::CalcWordWrapPosition`), with two differences. A word that only a
  whole row has room for starts the next row whole, also when that leaves the
  label alone on its row, where Dear ImGui would cut it beside the label or
  break it after punctuation. A word wider than a whole row, such as an
  address, starts where the text before it ends and is cut at each row's end
  by character (`ImFont::CalcTextSizeA`).

### Mentions

A mention is `@` and a connected player's name in a line's text. It is
plain text on the wire ([Network](network.md#chat)), so the line holds it as
sent and only how it is drawn differs.

- `ChatRoster::FindMentions` finds them when a line arrives
  (`ChatSync::DrainLines`, into `ChatLine::mentions`, offsets into the text).
  An `@` counts when it does not follow a name character, as in an address,
  and is followed by one. Its name is the longest folded match of a roster
  name that ends before a character that is not a name character, or at the
  text's end, so `@Ulf,` mentions `Ulf` and `@Ulfric` does not. Names are
  display names ([Network](network.md#sessions)); a bot's name with spaces
  matches as well. A mention is `local` when it names the local player.
- They are found once per line, against the roster of that moment: a name
  that is not connected then stays plain, and one that leaves later stays a
  mention.
- `ChatHistory` draws every mention in the heading face, as a line's label
  is, in the theme's text color, or, for a mention of the local player, in
  the [accent](#accent-color); without the heading face only the color sets
  it apart.
- A row that holds a mention is drawn in pieces (`DrawMentionRow`): a `Dummy`
  as wide as the row measures with its mentions, then each piece through the
  window's draw list in its own face. Rows without a mention stay one
  `TextUnformatted`, and a row out of view draws nothing.
- The heading face is wider than the text face, so a row is first broken as
  any other ([Lines and filters](#lines-and-filters)) and then, while its
  mentions make it wider than its room, gives up words from its end
  (`FitMentionRow`).
- A mention does nothing on its own: no notice, sound, or unread mark, and a
  click on it does nothing.

### Player list

- The filters' row ends with a chip button (`ChatStyle::DrawChipFrame`) that
  shows the person glyph of the [logo](#logo) font and the number of
  connected players, the local one included (`ChatRoster::GetPlayerCount`),
  or an offline state without a roster. A click opens or closes the list. The
  label is formatted only when the number changes.
- The list is a window of its own beside the open chat, on the right, or on
  the left when the screen has no room on the right
  ([Position](#position)), as high as the chat, in its menu frame and
  background. `ChatWindow` places and begins it as it begins the chat
  (`BeginFramed`), after the chat, so it has the chat's size from the same
  frame; `ChatPlayers` draws what is inside.
- A search field on the filters' row keeps names that contain its text, both
  folded as the [name list](#name-list) folds them. Tab stays in the field;
  Enter gives the keyboard back to the chat's field.
- The names follow, sorted by `ChatRoster`. Players Nearby chat reaches
  (`ChatRoster::CopyNearbyPlayers`, [Network](network.md#nearby-players)),
  so in the same interior at any distance, come first in the Nearby color;
  the others follow in `kDistantPlayerColor`. The local player is left out;
  the chip's count still includes it. A player's [role](#roles) mark
  (`ChatRoster::ListedPlayer::role`) stands before the name. With nothing to
  list, one row says why: no name matches, nobody else is online, or there is
  no roster.
- A name acts as a sender's name does in the lines: a click whispers the
  player, and a right click opens its [player menu](#player-menu). The
  keyboard then returns to the chat's field.
- A muted or vanished player has the logo font's muted or vanished mark at
  the list's right edge, both when both, and hovering them names the state.
  Without the logo font the list draws no marks; the
  [player manager](#player-manager) still lists both states. A vanished
  player within Nearby chat is still listed as nearby, since its Nearby lines
  still arrive.
- A button at the list's bottom opens the [player manager](#player-manager).
- The [chat in a trade](#chat-in-a-trade) has no players button and draws no
  list; a list left open shows again after the trade.
- Only the button closes the list. A chat closed by Escape, Enter, or a menu
  that opens by itself shows the list again, with its search, when it opens.
  Closing the list empties the search, and nothing of the list is saved.
- The wheel over the list scrolls the list only: `ChatHistory::DrawLines`
  leaves the wheel to it while `ChatPlayers::IsHovered`.
- Closed, the list costs the count. Open, `ChatRoster::CopyPlayersIfChanged`
  copies the roster only when its revision changes, and the search is matched
  then, on an edit, and when the players nearby change, which are copied every
  `kNearbySeconds` (0.5 s); `ImGuiListClipper` draws only the rows in view,
  and the frame reuses the [menu frame](#menu-frame)'s kept triangles, so the
  list's cost does not grow with the number of players.

### Size

- The open chat's lines are `[UI] ChatWidth` font sizes wide and `ChatLines`
  lines high ([Configuration](configuration.md#ui)), so the chat follows the
  overlay scale. The minimums keep room for the field beside the longest chip
  and for the name list under the filters.
- The player drags any of the open chat's four edges or four corners. The
  opposite edge stays in place: a top or right drag keeps the chat's
  [position](#position), and a left or bottom drag moves it
  (`ChatSize::GetCorner`). The grips are the strips the menu frame runs in,
  which hold nothing else to click, and each corner is its knot's square
  (`MenuFrame::Metrics::corner`); over them the cursor is the matching resize
  one. `ChatSize::FindEdges` finds them by position, since Dear ImGui items
  there would add to the window's content size.
- During a drag the size follows the cursor by the pixel, as the settings
  window's does; a drag in whole font-size steps looked like a low frame
  rate. The size stops at the limits and at the room on the screen
  (`ChatSize::GetRoom`, `GetUnits` in `ChatWindow.cpp`), which runs from the
  chat's [position](#position) to the right and top screen edges, or, during
  a left or bottom drag, from the edge held in place to the left or bottom
  screen edge, and leaves room for an open [player list](#player-list) on the
  side the chat grows toward. A configured size the screen has no room for is
  drawn at that room and stays in the file.
- Releasing the button settles the size on the nearest whole font size
  across and line (`ChatHistory::GetLinePitch`) up, so the history ends on a
  whole line, and applies it with `Config::Apply`, once and only if it
  changed, and logs it; a chat that loses focus mid-drag does the same
  (`ChatSize::Release`). After a left or bottom drag the same apply stores the
  [position](#position) for the settled size (`ChatPosition::Store`).
  `ChatSize` reads the configuration each frame outside a drag.
- The window has the dragged size in the frame the cursor moved:
  `ChatSize::Follow` runs before `ImGui::Begin` and gives the window both its
  width and height (`ImGui::SetNextWindowSize`), the lines' plus what is
  around them as measured in the previous frame (`ChatWindow::aroundHeight`).
  A window as high as its content would follow a frame late, and one as wide
  as its content would never get narrower. Only a new chat is as high as its
  content, for the one frame Dear ImGui hides a new window. `ChatSize::Grab`
  starts a drag after `Begin`, where the window's place is known.
- While the size changes, the latest line stays at the lines' bottom if it
  was in view (`ChatHistory::following`); lines scrolled back stay where they
  are. In a frame whose wrap width differs, by a drag or by the scrollbar
  showing or going (`ChatHistory::shownWidth`), `ChatHistory::DrawLines` lays
  the lines out twice: once under an empty clip rectangle to find their new
  height, then from where that scrolls them to, and passes that height to the
  next frame (`ImGui::SetNextWindowContentSize`), since Dear ImGui scrolls by
  what the lines took a frame ago. A new height alone needs no second layout:
  the scroll to the end (`ImGui::SetScrollHereY`) is applied at the height of
  the frame it lands in. A width drag so costs one more layout of the lines a
  frame (about 230 µs against 135 µs idle with 300 lines held).
- The closed chat's box is as wide as the lines and shows at most 6.

### Position

- `[UI] ChatPositionHorizontal` and `ChatPositionVertical`
  ([Configuration](configuration.md#ui)) place the lower left corner of the
  open chat's frame (`ChatPosition::Get`) at a percentage of the screen's
  width from its left edge and of its height from its bottom edge, with any
  fraction, rounded to the nearest pixel, so the place is the same share of
  any resolution, independent of the `Scale` setting.
- The chat stays at its position: its [size](#size) is limited to the room
  from that corner to the right and top edges, rather than the chat being
  moved back onto the screen. Only a chat at its minimum size, which the room
  does not hold, is moved until it fits (`KeepOnScreen`). The recent lines
  are placed from the same corner with the open chat's size as last measured,
  so they keep their column when the chat opens.
- The [player list](#player-list) goes right of the chat when the chat at its
  configured size and the list fit there, otherwise left when there is room,
  and otherwise right with the chat narrowed (`ChatWindow::ChooseListSide`).
  The side is chosen from the configured size, not the narrowed one, so it
  does not change back and forth.
- The settings window shows both as sliders in whole percents
  (`SettingsVisitor::FinePercent`); a fraction a drag stored shows rounded
  until the slider moves. The chat moves when a slider is released.
- The player also moves the open chat by dragging any part inside the frame
  that takes no click itself. `ChatPosition::Grab` runs after every item in
  the window, so `ImGui::IsAnyItemHovered` covers the filters, the chips, the
  field, and the scrollbar; a click that picked a sender's name
  (`ChatHistory::DrawLines`), one while a menu is open, and one over a resize
  grip (`ChatSize::Grab`) start no move. Moving starts past Dear ImGui's drag
  threshold, so a plain click moves and saves nothing. While moving, the
  cursor is the move one.
- During the drag the corner follows the cursor by the whole pixel and stops
  where the chat at its configured size touches a screen edge
  (`ChatPosition::Follow`), so a drag never shrinks the chat. `GetUnits`
  allows half a pixel, so a chat against the right or top edge keeps its size.
  The player list's side is chosen anew each frame.
- Releasing the button, or the chat losing focus mid-drag
  (`ChatPosition::Release`), stores the corner as exact percentages
  (`ChatPosition::Store`), applies them with `Config::Apply` only if they
  changed, and logs them. A value that already puts the corner on that pixel
  is kept, so a slider's whole percent survives a click on an edge.

### Window and input

- `[Controls] OpenChat` ([Configuration](configuration.md#controls)) during
  gameplay opens the chat and focuses its field; keypad Enter opens it only
  when bound. The key only opens: the chat closes by Enter and Escape. The
  whisper item in the [player menu in the world](#player-menu-in-the-world)
  opens it too, with that player as the send target. During a trade the chat
  is docked in the trade window instead ([Chat in a trade](#chat-in-a-trade)).
  Typing goes only to the chat, and keys already held keep acting until
  released.
- Escape closes an open menu, else the [player manager](#player-manager),
  else hides the name list, else discards the text typed since opening or
  since the last line sent ([Sending](#sending)) and closes the chat. Every
  key, console and menu keys included, goes to the chat while it is open, but
  a menu that opens by itself, such as dialogue an NPC starts or a loading
  screen, closes it and keeps the text.
- The chat uses the mouse, with the cursor hidden until the mouse moves
  ([Input focus](#input-focus)), so a player who only types sees none. After
  any click the keyboard returns to the field, except one in the player
  list's search field; a click outside the chat and the list does nothing.
- Open, it sits at its [position](#position), with the filter row, the lines
  at the chat's [size](#size), and the input row inside the
  [menu frame](#menu-frame), whose metrics pad the content clear of the
  knots. The wheel and Page Up/Down scroll the lines; new lines scroll down
  unless the history was scrolled back.
- Closed, the newest lines (at most 6) show for 10 s with no background
  and fade out over the last 2; with nothing recent nothing is drawn. They
  follow the filters, whispers included, so turning Global off keeps global
  chat off the screen. They have no frame, since its corners alone are taller
  than a line or two, and stay in the open chat's column.
- A soft shadow under the recent lines keeps them readable over bright
  scenes: `ChatWindow` draws the lines once per shadow offset, turns each
  copy's vertices black at a fraction of their alpha and shifts them
  (`MakeShadow`), then draws the lines over them, so it covers role marks and
  mentions without the lines' drawing knowing about it.
- The recent lines' box is pinned by its bottom and fitted to them by Dear
  ImGui from the previous frame's content. For the frame after a line leaves,
  the lines are moved down to its bottom (`KeepContentAtBottom`) rather than
  jumping up; for the frame after a line arrives, they are clipped at its
  bottom (`PushContentClip`), so the new line shows a frame later, all at
  once, with the last line's shadow whole.
- The open chat and the recent lines are each drawn in a `Theme::Scope`
  ([Theme](#theme)), so the chat has no colors or sizes of its own beyond
  `ChatStyle`'s channel and failure colors and chip frame. Lines are
  `ChatHistory::GetLinePitch` apart, closer than the theme's items.
- The field has `ImGuiInputTextFlags_CallbackAlways`, `CallbackHistory`, and
  `CallbackCompletion`, and its callback is where edits reach the composer
  and rewritten text returns to the field, in the same frame. Escape makes
  Dear ImGui put back the text the field had when it took the keyboard before
  that callback runs, so the callback ignores a frame with Escape pressed and
  the field is refilled from the composer's text. Keyboard focus is requested
  again whenever the field is not active, except while a menu is open or the
  mouse holds an item, whose click it would otherwise take.

## Mute and vanish

Goal: a player can stop seeing another player without leaving: mute hides
what it says, vanish removes its character from this world. Either is chosen
from a [player menu](#player-menu), in the chat's lines, the
[player list](#player-list), or the world, shows as a mark in the list, can
be undone there or in the [player manager](#player-manager), also while the
player is offline, and lasts across games on the same server. Non-goals:
telling the player, a mutual vanish (a vanished player still sees the one
who vanished it), muting or vanishing by name, a vanished player's Nearby
chat (mute covers chat), and admin moderation.

- `PlayerFilters` (`sync/filters/`) holds the current server's entries by
  session ID: a name, `friended` ([Friends](#friends)), `muted`, and
  `vanished`, each independent of the others. It reads them from
  `Network::PlayerFilterStore` for `Network::GetServerUrl` and again
  whenever the connection generation changes, so a session is never looked
  up on another server's list ([Network](network.md#player-filters)). A
  revision, the sum of its own change count and the generation, tells
  readers when to copy again; it is two atomic loads.
- `SetMuted` and `SetVanished` change one flag and save the entry, without a
  [notice](#notices), since the menu item, the list's marks, and the hidden
  lines or character already show the change. `SetVanished` then publishes
  it ([Network](network.md#vanish)). A player without a session cannot be
  either. At 1024 vanished players on a server another vanish is refused
  with a notice giving the limit. A failed save posts a notice that the
  change lasts until the game exits.
- The [player menu](#player-menu) ends with a mute or unmute item and a
  vanish or unvanish item, each with a tooltip saying what it does and that
  the player is not told. The local player (`ChatRoster::GetLocalSession`)
  and a player without a session get none.
- Muting hides the player's lines on every channel, whispers included, in
  the open chat and the closed chat's recent lines, and what it said before
  the mute too; unmuting shows every line still held. `ChatHistory` keeps a
  `muted` flag per line from the sender's session (zero for the local
  player's own lines), set when a line arrives and recomputed for all lines
  only when `PlayerFilters`' revision changes. A muted line marks no filter
  unread. A muted player's whisper does not become a whisper partner
  (`ChatSync::DrainLines`).
- Muting also silences the player's voice: `VoiceChatSync` leaves a muted
  speaker out of the mixer, so its frames are dropped on arrival
  ([Voice chat](voice-chat.md#playback)). A vanished player's voice ends with
  its character, since the server stops relaying it.
- Vanishing is the server's: the client stops receiving the player
  ([Interest and delivery](interest-and-delivery.md#subscriptions)), so its
  remote actor leaves as for any `EXCLUDED` peer and returns, if in range,
  when the vanish is undone. It does not hide the player's chat.

### Player manager

Where the friends, muted, and vanished players of the current server are
listed, offline ones included, and undone.

- `ChatPlayerManager` draws it and `ChatWindow` places and begins it
  (`BeginFramed`) after the chat and the player list, or alone, in the menu
  frame. It opens in the middle of the screen (`ImGuiCond_Appearing`) and can
  be dragged by any empty part while open; every opening starts in the
  middle again. The open chat's windows have
  `ImGuiWindowFlags_NoBringToFrontOnFocus`, so a click in the chat or the
  list leaves it in front; the list's button brings it to the front.
- It holds a title with a close button, a chip tab each for friends, muted,
  and vanished players, and a rule. Every opening shows the friends tab;
  nothing of the window is kept once it closes.
- A tab with players has a search field that keeps the names containing its
  text, folded as the [name list](#name-list) folds them. The search stays
  across tabs and empties when the window closes; Tab stays in the field.
  Then a row per player of the tab, the connected ones first, each group
  sorted by folded name: an online dot (`kOnlineColor` while connected) whose
  tooltip tells which; the name, the roster's while online and as kept
  otherwise; and a button that removes the friend, unmutes, or unvanishes, as
  the menu does, and removes the row at once. An empty tab, and a search
  matching nobody, show a line saying so. `ImGuiListClipper` draws only the
  rows in view.
- A name acts as a sender's name does in the chat's lines: a click whispers
  the player while it is online, and a right click opens its
  [player menu](#player-menu) (`##ManagePlayersMenu`), offline too. An offline
  player has no slot, so its menu has no level or place and whisper is
  disabled. Whisper sets the chat's target as the list's does, and the window
  stays open. The chat counts the menu among its open menus
  (`ChatPlayerManager::IsMenuOpen`), so Escape closes it first and the field
  leaves the keyboard to it.
- It copies the roster and `PlayerFilters` only when their revisions change,
  and rebuilds the tab's rows then and when the tab changes.
- Escape closes it before the chat, from any field. Its close button closes
  it alone. Closing the chat, by Escape, Enter, or a menu that opens by
  itself, closes it too, and it does not show again until the list's button
  is pressed.
- `[Controls] ManagePlayers` ([Configuration](configuration.md#controls))
  during gameplay opens it alone, without the chat: the chat's window takes
  focus and `ChatWindow::OnFocusKey` marks it manager-only (`managerOnly`,
  atomic, as the input thread writes it). The cursor shows at once in the
  middle of the screen (`HidesIdleCursor` false), the recent lines show as
  with the chat closed, and the key only opens it. Escape closes an open
  menu, else the window, and its close button closes it; either gives input
  back to the game. A whisper, by a name's click or its menu, closes it and
  opens the chat with that player as the target, as a whisper from the world
  does. The chat's key does nothing while it is open. The wheel over it
  scrolls it and not the chat's lines (`ChatPlayerManager::IsHovered`). After
  a click the keyboard returns to the chat's field, except in its search
  field.

## Friends

Goal: a player can keep other players to find again: added from a
[player menu](#player-menu), listed with whether they are online in
the [player manager](#player-manager), kept across games on the same server,
and announced by a [notice](#notices) when they connect. Friendship is one
way: the player is not told and need not add back. Non-goals: requests or
mutual friendship, a mark for friends in the player list or chat lines (only
the menu, the tab, and the [nameplate](#nameplates) color show it), a notice
when a friend leaves, friends across servers (a session ID names a player on
one server), and a limit, since nothing about friends is sent.

- `SetFriend` sets `friended` and saves the entry as `SetMuted` does,
  without a notice. A friend may also be muted or vanished; removing
  the last of the three drops the entry.
- The [player menu](#player-menu) has an add or remove friend item right
  after whisper. The local player and a player without a session get
  neither, as for mute and vanish.
- `PlayerFilters::CollectArrivedFriends`, called by the
  [notice stack](#notices) every frame, copies the roster only when its
  revision changes (`Network::CopyRosterIfChanged`) and keeps the sorted
  session IDs of the previous copy. A roster without the local player,
  before a connection's first `Roster` or after a disconnect, clears them.
  The first roster with a new local slot is a connection's first and lists
  who was already online, so it announces nobody; after it, each friend
  whose session was not in the previous copy gets a notice that it is
  online, muted and vanished friends too. A friend who leaves and comes back
  between two copies is not announced; one who reconnects later is announced
  again.
- The same pass refreshes the kept name of any entry, friend or not, whose
  player appears with another name, the first roster included, and saves
  it, so an offline row shows the name last seen.

## Roles

How the game shows the [role](network.md#roles) a server's admin gave a
player, which owns the feature's goal and acceptance criteria. Non-goals: a
name in the role's color in chat, since it would hide the line's channel,
which the label's color tells; a mark in the [name list](#name-list), the
trade window, or notices; and a setting that hides roles.

- `ChatRoster` holds the server's roles with the roster. On each roster
  revision it makes every role's `RoleMark`: the role's color and its
  letter, the name's first character in upper case (`Utf8Case::AppendUpper`,
  so of the scripts it covers; any other character as it is).
  `FindRoleMark` gives the mark of a role ID and `FindRole` the role, both
  empty for 0 and for an ID the server no longer lists.
- `RoleBadge` (`ui/widgets/`) draws both forms as chips in the role's color
  (`ChatStyle::DrawChipFrame`). The mark is a square chip holding the letter
  in the heading face: `DrawMark` on any draw list at a given size and alpha,
  which the [nameplates](#nameplates) and the [player list](#player-list)
  use, and `Mark` as an item as high as a line of text, which the
  [chat's lines](#lines-and-filters) use. `Name` is the whole badge, the
  role's name in a chip, for the [player menu](#player-menu).
- The letter is centered by its outline's exact bounds
  (`Fonts::FindHeadingBounds`, cached per letter), not by the baked glyph's
  box, which widens to whole pixels and so shifts a box-centered letter by a
  pixel with its shape and with the mark's size, which changes with a
  nameplate's distance. A letter the heading font lacks comes from a fallback
  font and is centered by its baked box.
- The player menu shows the role its player holds when it opens: the
  roster's for a connected player whose entry has the same session, else the
  identity's own, so a line of a player who left still names the role they
  had. A player without a slot, an offline row of the
  [player manager](#player-manager), shows none.
- A role given, taken, renamed, recolored, or deleted shows without a
  reconnect: each arrives as a roster change ([Network](network.md#roles)),
  and every reader keys on the roster's revision.

## Player menu

Goal: wherever another player's menu opens, a right click on its name in
the chat's lines or the [player list](#player-list) or Activate on its
character ([below](#player-menu-in-the-world)), it looks the same and says
who the player is and where: its name, level, and place, then whisper,
[trade](#trade-window), [friend](#friends), and
[mute and vanish](#mute-and-vanish) items. Non-goals: the player's race
(its character shows it, and a modded race the local game lacks could only
be named by its local fallback), the name of a place whose plugin the local
game lacks (names are not on the wire), and following a level-up or a move
while the menu is open.

- `ChatPlayerMenu` is the menu's content. `ChatHistory`, `ChatPlayers`,
  `ChatPlayerManager`, and `WorldPlayerMenu` each hold one, call `Open` with
  the roster entry when they open their popup, and `Draw` inside it, which
  reports a whisper for each to handle.
- It starts with the roster's name in the heading face and the accent,
  followed on its line by the level (`kPlayerMenuLevel`) in the smaller
  secondary size (`kSecondaryTextScale`), none at level zero. Below come the
  player's [role](#roles) badge when it has one and a line naming the
  player's place after the [logo](#logo) font's location pin, or that the
  place is unknown when it does not resolve locally; there is no place line
  for a player outside the world. The items follow. Whisper is disabled for a
  player without a slot, which only an offline row in the
  [player manager](#player-manager) opens the menu for.
- `Open` asks `PlayerDetails` (`sync/chat/`) for the player's slot,
  replacing any earlier request. The next pass serves it on the game thread
  ([Sync pipeline](sync-pipeline.md#frame-sequence)): a peer with an actor,
  which receives every appearance revision, is answered from its latest
  appearance; any other player, the local player included, is asked of the
  server with `Network::RequestInspection`
  ([Network](network.md#inspection)), since an appearance kept after a peer
  left range may be old. Without a connection the answer is empty.
- The level is `Appearance.level`, zero before the player sent an
  appearance.
- The place (`Details::place`) comes from the player's latest context
  ([Network](network.md#checkpoint-and-context)): the newest received
  checkpoint's for a peer with an actor, the `Inspection`'s otherwise. Its
  name is the first full name found among the interior cell, the location,
  and the worldspace, each resolved with `FormLookup::ResolveForm` to its
  form type, so an exterior is named by its location and an unnamed
  interior or location falls back to the next. None resolving, as when the
  player's plugins are not loaded, gives an empty name, shown as unknown;
  no reference at all leaves `place` unset.
- The local answer also says whether the player's character is near
  enough to trade (`Details::inTradeRange`, [Trade](trade.md#invitation)); a
  server answer never is, since a player without a character is not near.
- `PlayerDetails` keeps only the latest request, with its slot and session,
  and its answer, under one lock; an answer for another slot is dropped. It
  also keeps the level and place of each answer that has either, by the
  requested player's session (`FindKnown`), for the rest of the run. A
  session survives reconnects, so a reused slot never shows another
  player's details.
- `Open` shows the known details at once and still asks for fresh ones,
  which replace them when they arrive; an empty answer, such as one without
  a connection, keeps them. While the answer is due the menu reads it once a
  frame; with nothing known it holds the place line's height, so the menu
  does not grow under the cursor. After `kDetailsWaitSeconds` (2) of clamped
  frame time it stops waiting and shows the known details or the name alone,
  as for a reply the server's rate limit dropped.

## Player menu in the world

Goal: activating another player's character, or the mount it rides, with
Skyrim's Activate key opens that player's [player menu](#player-menu) at
the crosshair. Non-goals: another crosshair prompt (Skyrim's own prompt and
name stay) and gamepad navigation.

- `RemoteActorActivation` (`runtime/`) is a `TESActivateEvent` sink on
  `ScriptEventSourceHolder`, registered once data is loaded. Remote actors
  keep their blocked activation, so Skyrim starts no dialogue, but the event
  is still sent ([Remote actor](remote-actor.md#spawn)). It keeps the local
  player's activations of a registered remote actor
  (`ActorRegistry::IsRegistered`) that is not dead (`ActorRegistry::IsDead`),
  so a body opens no menu, and stores the actor's form ID with
  `SyncCoordinator::QueueLocalActorActivation`, the latest one winning, since
  Skyrim activates on a worker thread.
- The next pass finds the peer whose actor or mount has that handle
  ([Sync pipeline](sync-pipeline.md#frame-sequence)) and gives its slot to
  `PlayerActivation::Activate`, which looks the player up with
  `ChatRoster::Find`. A player the roster does not list opens nothing.
- `WorldPlayerMenu`, registered after the chat, takes the request when no
  window holds focus and takes focus with `WindowManager::Focus`; otherwise
  the request is dropped. With focus the cursor comes up in the middle of the
  screen ([Input focus](#input-focus)), and the menu, a theme popup, opens
  with its corner there, over the crosshair. The chat stays closed.
- Picking an item, a click of either button outside the menu, Escape, and
  the keyboard key mapped to Activate (`ControlMap::GetMappedKey`) close it
  and give input back to the game, as does a menu taking over input. Escape
  and Activate are release keys, so neither the game nor the menu sees them:
  Escape does not open the journal, and Activate does not activate again.
- Whisper releases focus and leaves the player with
  `PlayerActivation::RequestWhisper`. The chat takes it the next frame, takes
  focus, and opens with the whisper as its [send target](#send-target), its
  cursor hidden as when Enter opens it.
- Without an activation the menu and the chat each take one uncontended lock
  a frame for their request; nothing scales with the number of remote actors.

## Trade window

Goal: the overlay half of [Trade](trade.md): an invitation shows without
taking control from the player, and an open trade takes the screen: one
fixed panel with the player's items and both offers, beside the hovered
item's model and numbers as the vanilla inventory shows them. Non-goals:
gamepad navigation, an offer's total weight and value, sorting the peer's
offer, placing items in the grids, item sizes on the grid (every line takes
one cell), and zooming the model.

The windows read `TradeSync::View`, copied when its revision changes, and
send commands (`RequestTrade`, `AnswerInvitation`, `Cancel`,
`SetOfferCount`, `Accept`) that the next pass applies on the game thread;
neither touches an engine object.

### Prompt

- `TradePrompt`, registered before the settings window so its keys come
  first, draws while a trade is requested, invited, or inviting, as a
  `MenuFrame::BeginPrompt` panel under the compass. It has no inputs and
  never takes focus to show.
- It has two centered lines: the message, then the key hints and the seconds
  left.
- Invited: the inviting player's name and the invitation, then accept and
  decline hints, each with its configured key's label
  ([Configuration](configuration.md#controls)). Requesting and inviting look
  the same, so the prompt keeps its size when the server's invitation
  arrives: the other player has yet to answer, then a withdraw hint with the
  decline key; the seconds stay at the whole `TradeSession::kInviteSeconds`
  while requesting. The seconds take the width of the longest count, so the
  centered panel does not shift. An unbound action's hint is left out; the
  invited player then answers from the player menu.
- `IsFocusKey` takes `AcceptTrade` while invited and `DeclineTrade` while
  invited or inviting, and nothing otherwise, so the keys stay the game's.
  While requesting there is no trade ID to cancel yet, so the withdraw hint
  shows but its key stays the game's. The key that gave focus answers, and
  the prompt gives focus back in the same frame, so neither the game nor
  another window sees it.

### Window

- `TradeWindow` takes focus with `TakeFocus` once the view is open, also
  from the chat or the player menu, and takes the whole display: the
  [backdrop](#backdrop) blurs and darkens the game behind every overlay
  window, and a click on it does nothing. The cursor shows. A window kept
  from focus by a game menu retries each frame and gives the trade up with
  `FAILED` after `kFocusWaitSeconds` (2) of clamped frame time; one that had
  focus and loses it, other than by
  [lending it to the chat](#chat-in-a-trade), cancels with `MENU`.
- The window is laid out as three columns centered together on the display:
  the [chat](#chat-in-a-trade); the trade itself, a framed panel that cannot
  be moved or resized (`MenuFrame::WindowLayout::fixedPosition`); and the
  [preview column](#preview-column), right of the panel as the inventory menu
  shows its item.
- The layout is drawn at the overlay's font size where the display holds it,
  and otherwise at the largest font size that does (`GetLayoutFontSize`,
  pushed with `Fonts::SizeScope` before the theme, so every length in font
  sizes follows it), so it looks the same on 16:9 and every wider display
  rather than narrowing the chat or the preview.
- The title names the other player; Cancel and Escape close the window. The
  left column holds the player's items, and the right column the peer's offer
  above the player's own, each a grid.
- Each offer is a grid of 5 × 3 cells, one line per cell in offer order; the
  player cannot move items in it. An offer holds at most 15 lines
  ([Trade](trade.md#offers-and-acceptance)), so every line is in view without
  scrolling; `TradeWindow.cpp` asserts at compile time that the grid holds
  `kMaximumTradeItems`. The cell is the largest whole pixel size, at least
  `kMinimumCellFonts`, that fits both grids in the panel and leaves the item
  list its width; cells are separate wells `kCellGapFonts` apart.
- A cell shows the item's [thumbnail](#3d-preview) on the well's color
  (`kItemColor` = `kWellColor`), which must be opaque, since the still is
  captured on it, and the count above one. While thumbnails are off
  (`[UI] ItemPreview`) or unavailable, the cell shows the name instead; while
  one is captured, only the background. Hovering a cell frames it in the
  accent and shows the item in the preview column at once.
- The player's offer: a clicked item appears at once; a line the other game
  [cannot receive](trade.md#offers-and-acceptance) leaves it when that game's
  report arrives. Clicking a cell that offers one item removes its line at
  once (`TradeSync::SetOfferCount` with 0). Clicking a cell that offers more
  opens a popup with the name, a count field clamped to what the player
  holds, and a remove button; the field is left out if the player comes to
  hold only one while it is open. Stepping down from 1, or entering 0,
  removes the line and closes the popup; typed text applies only on Enter or
  focus loss, so a 0 on the way to another count does not. Escape closes the
  popup, and it closes when its line leaves the offer.
- The peer's offer: names as the local game resolves them; lines it cannot
  receive are left out.
- While the own offer holds `kMaximumTradeItems` lines, a full mark in
  `Theme::kWarning` follows the own grid's heading.
- While a player has accepted, its grid is tinted and framed in
  `Theme::kPositive`, and its heading says it accepted, so the state does not
  rest on color alone.
- The player's items: a search field (case-insensitive, by name) and a
  category, matched again only when either or the inventory changes; rows
  clipped to the visible ones, with counts above one. A click on an offered
  row does what a click on its offer cell does, and a click on a single item
  not yet offered offers it. A click on a stack not yet offered opens a popup
  with the name, a count field focused with its text selected, clamped to 1
  to what the player holds, and a button that offers; only the button or
  Enter sends the count (`TradeSync::SetOfferCount`); Escape, also while
  typing, and a click outside close it without a change, and it closes when
  the inventory changes, since its row may then be another item. Right clicks
  do nothing. Offered rows are highlighted. Disabled: rows not yet offered
  while the offer is full (the full mark shows why), rows that cannot be
  offered, also those the other game
  [cannot receive](trade.md#offers-and-acceptance), and rows whose plugins
  the other game has not yet reported on (`InventoryLine::checking`,
  [plugin check](trade.md#plugin-check)); while any is checking
  (`View::checkingPlugins`) a pulsing note follows the item list's heading. A
  hovered row, also a disabled one, shows its item in the preview column at
  once.
- The footer holds Accept, disabled while blocked or accepted, and Cancel.
  Acceptance shows on the grids alone. `TradeSync`'s reason Accept is
  unavailable (nothing offered, the offer sending, a report awaited) is the
  disabled button's tooltip rather than a line, since it mostly lasts a
  round trip after each edit and would flicker. Escape, when no field or
  popup claims it, cancels as Cancel does.
- A trade that ends releases focus; a window that cancelled waits for the
  view to close without taking focus again.

### Chat in a trade

Goal: players can talk while trading without cancelling it, with the chat
in view beside the trade. Non-goals: the [player list](#player-list) and
the [player manager](#player-manager) during a trade, since the list would
cover the panel (names in the lines and Tab still pick a whisper target, and
`@` still mentions), and resizing or moving the docked chat.

- In each frame the trade window draws its panel, it gives the chat its
  layout's left column and font size (`TradeDock::Set`; `TradeDock::Get`
  returns it for that frame only). The chat window is registered after the
  trade window, so it draws there in the same frame, as the open chat does
  but without the players button, at the column's size rather than
  `[UI] ChatWidth`, `ChatLines`, and the chat's position, and whether or not
  it holds focus. The closed chat's recent lines are not drawn, and lines
  arriving count as read on the channels shown.
- The trade lends the chat focus ([Input focus](#input-focus)) on
  `[Controls] OpenChat` while no trade field or popup has the keyboard
  (`TradeWindow`'s `keysFree`, set after each draw), and on a left or right
  click on the docked chat (`WindowManager::Borrow`). The click is read
  before the chat's items, so it goes on to what it hit. While a trade popup
  is open, Dear ImGui reports the chat not hovered, so a click there only
  closes the popup. `ManagePlayers` and other windows' keys stay inactive.
- Without focus the docked chat only shows: its items are disabled at full
  opacity (`ImGui::BeginDisabled` with `DisabledAlpha` 1), its field asks
  for no keyboard and gives up one it still holds
  (`ChatInput::Frame::passive`), Page Up and Down leave its lines, and the
  wheel scrolls them only under the cursor.
- When the docked chat appears, the send target becomes a whisper to the
  other trader (`TradeSync::GetOpenPeer`); the player can change it, and
  focusing the chat again keeps the change and the lines' scroll.
- While lent, the trade handles no Escape, also in the frame the chat gives
  focus back, so the Escape that leaves the chat does not cancel the trade.
  A line sent keeps the docked chat focused, with the field emptied and the
  keyboard back in it, whatever `[UI] CloseChatOnSend` says
  (`ChatInput::Frame::stayOnSend`). Escape, and Enter in an empty field while
  that setting is on, leave it as they close the open chat
  ([Sending](#sending)), returning focus to the trade.
- A left or right click on any of the trade's windows (`NoteClick`: the
  panel, the model, the card) takes focus back with `TakeFocus`, keeping the
  chat's text, and the click goes on to what it hit. A click that only closes
  a chat menu, or one on the backdrop, does neither.
- A game menu taking input releases both, and the trade cancels with `MENU`
  as without the chat. A trade that ends while the chat holds focus leaves
  the chat open at its own position and size.

### Preview column

The vanilla inventory's item display, right of the panel: the item's model
above a card with its numbers. It replaces the item tooltips; the only
tooltip left in the window is Accept's blocker.

- It shows the item last hovered in the item list or either grid, and keeps
  it while the cursor is over the backdrop or the panel's empty parts. Until
  the first hover the square is empty and the card is left out. A row of the
  player's is found again by form and name when the inventory changes, since
  rows can move, and the column empties when it is gone or when the peer
  takes the shown line out: a shown peer line is found by `PeerLine::id`,
  which `TradeSync` keeps while the peer offers the same item and count.
- The model's square is the column's width, leaving the card a minimum
  height. It shows the item's [model](#3d-preview) over the backdrop, as the
  inventory menu does, spinning and turned while dragged with the left
  button. With `[UI] ItemPreview` off it stays empty.
- The card is a framed window under the square (`ui/trade/ItemCard`) laid
  out as the vanilla inventory's item card. As in the inventory menu, its top
  stays put and its bottom follows the content: `MeasureItemCard` lays the
  card out without drawing before the window begins, and the window is that
  tall, up to the panel's bottom, where a longer card scrolls. The content
  scrolls in a child exactly the content area's size, because a window clips
  its content only half its padding from its edge. Whether the card scrolls
  comes from the measurement, not from ImGui: ImGui truncates window and
  child sizes to whole pixels and could find a fitting card a pixel too
  tall, and its scrollbar would then narrow the text into more lines. It
  shows the item's [details](trade.md#item-details):
  - the name in capitals (`Utf8Case`), with the count after it for more than
    one, as the vanilla item list writes counts;
  - in `Theme::kWarning`, why a row cannot be offered (the
    [offer blocks](trade.md#offers-and-acceptance)), before the stats, so it
    is read first;
  - damage or armor rating of one item, then weight and value of the whole
    count it shows (held for an item list row, offered for a grid cell), so a
    stack and a single item read the same;
  - each effect as the game words it, with every number highlighted as the
    game shows magnitudes, an undiscovered ingredient effect without
    highlights, then the charge as current over maximum.

  The card shows no equipped, favorite, or enchanted note: an enchanted item
  is told by its effects and its glow.

## Voice chat

Goal: a player sees who is speaking, whether it is heard itself, and can set
how loud each player is, without leaving the game. How voice is captured,
carried, and played is in [Voice chat](voice-chat.md).

- A speaking peer's nameplate has the [logo](#logo) font's speaker glyph
  before its name, in the name's color. The glyph is static: it shows whether
  the peer is heard, not how loud. `NameplateSync` copies
  `VoiceChatSync::IsSpeaking` into each plate (`Plate::isSpeaking`), so a
  plate shows it only while in sight and in range, as its name.
- The [player list](#player-list) draws the same glyph in the accent before a
  speaking player's mute and vanish marks.
- Without the font, neither shows the speaker.
- `VoiceIndicator` shows the [logo](#logo) font's microphone glyph in the
  accent while the player is heard; without the font it shows nothing. It
  draws on the background draw list like the nameplates, under every window,
  but is registered after the [trade window](#trade-window), so it draws over
  the trade's [backdrop](#backdrop) and stays visible while trading wherever
  the trade's windows leave the screen free. It shows nothing outside a game,
  while `UI::IsGameViewShown` is false, or with `[Voice] Indicator` off
  ([Configuration](configuration.md#voice)).
- `[Voice] IndicatorPositionHorizontal` and `IndicatorPositionVertical` place
  the microphone's lower left corner as the chat's [position](#position)
  settings place the chat's. The microphone is then moved back until it is
  wholly on the screen. It does not follow a moved chat, and it has no drag:
  it takes no input.
- The settings window shows the two positions as `FinePercent` sliders, only
  while `Indicator` is on. While the pointer is over either slider or
  dragging it, the window calls `UI::PreviewVoiceIndicator` with its draft's
  positions each frame, and the indicator draws there in its next pass, heard
  or not, in a game or not, instead of at the live position, though the
  setting applies on release like the chat's.
- The `[Voice]` page of the [settings window](#settings-window) draws two
  rows of its own. An audio device (`SettingsVisitor::AudioDevice`) is a combo
  of the system's default, then the devices `VoiceChatSync::CopyDeviceNames`
  lists, with a saved device that is not plugged in kept in the list; opening
  it asks the audio engine to list the devices again. The input meter
  (`SettingsVisitor::InputMeter`) is a bar of the microphone's level from
  `VoiceChatSync::GetLocalStatus`, which `InputVolume` does not change, in the
  accent while the player is heard, with a mark at `ActivationLevel` under
  voice activation. While the microphone is closed, as without a connection,
  the bar stays empty. While `Enabled` is on, the microphone is closed, and
  `LocalStatus::relayUnreachable` ([Voice chat](voice-chat.md#relay-link)), a
  line under the `Enabled` row, from the page's left edge, tells the player
  in `Theme::kNegative` that voice chat is disabled because the voice relay
  cannot be reached.

## Settings window

Players change `PlayerHub.ini` ([Configuration](configuration.md)) in game and
see the result at once. The game is not paused; the world keeps running and
peers keep seeing the player.

- `[Controls] OpenSettings` opens it during gameplay as a focus key and closes
  it again as a release key ([Input focus](#input-focus)); a changed key
  applies once bound. The close button or Escape also closes it; Escape first
  goes to an active text field, open combo or list, or a
  [key binding](#key-bindings) waiting for a key. Any menu taking input
  closes it too.
- Every opening starts from the live configuration; closing discards unapplied
  outfit edits and text still being typed. The section that was open stays
  open, except that an opening with no display name applied shows
  `[Connection]` with the keyboard in its field.
- It is centered the first time it opens in a game session, with a minimum
  size. Later openings keep the position and size it was closed with; they
  live in ImGui's window state only, so a restart centers it again. It is in
  the [theme](#theme) and the [menu frame](#menu-frame), without a title bar:
  it is dragged by any empty part, resized from its edges and its four
  corners (the upper ones by [`BeginWindow`'s grips](#menu-frame)), and uses
  the mouse. The line ImGui draws along a hovered or dragged edge is made
  transparent (`ImGuiCol_SeparatorHovered` and `ImGuiCol_SeparatorActive`
  around `Begin`), so resizing shows the resize cursor only.
- A sidebar under the [logo](#logo) lists one item per file section in schema
  order: `[Connection]`, then `[DefaultOutfit]`, `[UI]`, `[Controls]`,
  `[Gameplay]`, `[Voice]`, and `[Advanced]`. Each section's title is the one
  its schema `Section` call gives ([Schema](configuration.md#schema)). The
  open section's item is marked with the accent strip of
  `Controls::StripedBox`. The connection status stays at the sidebar's bottom
  in every section (`ConnectionControls::DrawSummary`).
- The open section's title and the group names inside a section are in the
  heading face; everything else is in the text face.
- The page beside it shows the open section: its title, the section's comment
  where the schema has one, and its settings. Only the settings scroll; the
  title and the footer stay in view, and content stays clear of the frame's
  knots.
- Widgets come from the same `Config::Describe` visitor that reads and writes
  the file, so a new setting appears without UI code. A row is the label the
  schema gives the setting ([Schema](configuration.md#schema)) and its
  control: a switch for a bool, a slider for an unsigned value or a
  percentage, a text field for a string, an item picker for a form, a combo
  for a choice, a button showing the key for a key
  ([Key bindings](#key-bindings)), and swatches for a color
  ([Accent color](#accent-color)). Group comments head their rows in muted
  capitals (`Utf8Case::AppendUpper`)
  ([Configuration](configuration.md#settings)). `[UI] Language` is a combo of
  the loaded languages ([Localization](#localization)).
  `[Connection] DisplayName` is drawn beside the connection button instead of
  as a row ([Connection controls](#connection-controls)), and a `Layout`
  setting, as the chat's [size](#size) is, is not drawn at all.
- A row the schema shows only while a value is true (`ShowNextIf`,
  [Configuration](configuration.md#schema)) is left out while it is false,
  and its invalid text is dropped, so it does not come back invalid; after
  `ShowRestIf`, so are the rest of the section's rows and group headings.
  `[Voice]` so shows only `Enabled` while it is off.
  `[Connection]` so shows `ServerAddress` and `ServerTls` only while
  `CustomServer` is on
  ([Official and custom server](configuration.md#official-and-custom-server)):
  `ServerAddress` as a field without a label, since the switch above it
  already names it, and `ServerTls` as a switch row under it. The official
  server's address is never drawn.
- A setting's comment is not drawn; it is the tooltip of the row's label, and
  of a text field while its value is invalid.
- A number's slider runs from 0 to its maximum and shows the value. A drag
  or a typed value is rounded to the nearest step the schema gives; a value
  from the file off the step shows as it is until the slider moves. Where
  the schema names zero ([Configuration](configuration.md#schema)), it shows
  that name at 0: the name is the slider's format, which Dear ImGui draws as
  it is when it has no conversion. Ctrl and a click turn it into a field, and
  a typed value is clamped to the range.
- A fine percentage's slider (`FinePercent`) runs from 0 to its maximum in
  whole percents and shows the value rounded; the value can hold a fraction
  set elsewhere. A percentage's slider runs between its limits; a dragged or
  typed value is clamped and rounded to the nearest step from the minimum.
- Edits in every section but `[DefaultOutfit]` take effect and are saved as
  they are made, with no button: a switch, combo, or swatch at once, a key
  when it is bound, a slider when it is released, so a drag neither saves the
  file every frame nor resizes the window's content under the cursor, a color
  from the picker when it is released or its hex field's editing ends, and
  text when editing ends, on Enter, a click elsewhere, or picking another
  section, so a half-typed address never reconnects. Escape in a text field
  puts back the text it had when editing started. Text that does not parse is
  marked invalid with a line under the field saying so and leaves the setting
  at its last valid value, while other edits still apply. A line below the
  settings appears only while the file could not be saved.
- The footer's buttons share one width, the widest their labels need.
- `[UI]`, `[Controls]`, `[Gameplay]`, `[Voice]`, and `[Advanced]` have a
  footer with a Defaults button, enabled while a setting the page shows
  differs from the built-in one (`Config::GetDefaultConfig`); the save
  failure line shares the footer. A click restores the page's settings at
  once and applies them, without a confirmation; a listening
  [key binding](#key-bindings) stops listening, as on any click. `[UI]` keeps
  the chat's [size](#size), which the page does not show. `[Connection]` has
  no footer.
- `[DefaultOutfit]` keeps its edits until Apply, since an outfit is put
  together from several picks and each apply can publish new equipment and
  appearance revisions to peers. Its footer has Defaults, enabled while the
  edits differ from the built-in outfit, then Revert and Apply, enabled while
  they differ from the live outfit, Apply as the primary button. Revert
  returns to the live outfit and Defaults to the built-in one, without
  applying. All three leave the other sections alone, and an edit applied in
  another section leaves pending outfit edits pending. The footer reports
  unapplied changes, a successful save, or a failed one.
- `SettingsWindow` holds one draft `Config::PluginConfig`. After drawing, it
  applies the live configuration with the draft's other sections whenever they
  differ and no slider or color picker is active, and Apply does the same
  with the draft's `defaultOutfit`.
- The page is drawn before the sidebar, in one `Config::Describe` pass that
  also collects the section names the sidebar lists. A picked section
  therefore shows from the next frame, after a text field edited until the
  press has seen its editing end.

### Accent color

The player picks the [theme](#theme)'s accent on the `[UI]` page, so the
settings window, the open chat, and their lists can match a modpack's look.
`[UI] AccentColor` ([Configuration](configuration.md#ui)) holds it, and
`Config::SettingsVisitor::Color` passes the swatches
`Config::UISettings::kAccentSwatches` with it.

- The `AccentColor` row has the swatches, then a custom swatch. The grey
  swatch is the one whose `Accent` [frame](#menu-frame) matches the `Vanilla`
  frame, so the widgets can match the game's grey frame.
- The swatch of the current color is ringed; the custom swatch shows that
  color while it is not a swatch's. Hovering a swatch shows its `#RRGGBB`.
- A click on a swatch applies its color. A click on the custom swatch opens a
  picker below it: a saturation and value square, a hue bar, and a hex field,
  without alpha. A drag or a typed color shows in the custom swatch at once
  and applies on release or when the hex field's editing ends. Escape leaves
  an active hex field first, then closes the picker, then the window; a click
  outside also closes it.
- The new accent shows everywhere from the next frame, including the
  `Accent` [frame](#menu-frame). The status panel and state colors stay as
  they are.

### Key bindings

The `[Controls]` page binds each key ([Configuration](configuration.md#controls))
by pressing it, as games' control menus do.

- A row's button shows its key's label, or that it is unbound. A click on
  it, on the press, makes it listen, which the button shows in the accent.
  Nothing is added under it, so the rows below stay in place.
- The next key pressed is bound and applied at once, and listening ends.
  Escape, the key it already has, and any mouse click end it without a
  change; a click on another key's button makes that one listen instead.
- Backspace unbinds the action and applies at once; on an unbound action it
  ends listening without a change. `OpenSettings` and `OpenChat` are the only
  ways to open their windows, so Backspace there restores the default,
  `F4` or `Enter`, instead, checked against the other actions like a
  pressed key.
  `SettingsVisitor::Key` passes the value Backspace sets, `a_cleared`: 0 for
  an action that can be unbound.
- A reserved key, one without a name, shows a line under the button that it
  cannot be bound, and a key another action has a line naming that action by
  its row label; listening goes on, so the player
  presses another key or cancels.
- Losing focus or reopening the window ends listening.
- The page's Defaults button ([Settings window](#settings-window)) restores
  every key.
- `SettingsEditor` collects every key setting of the `Config::Describe`
  pass, and after the pass starts or ends listening for the frame's clicks,
  then takes the key pressed. After the pass, so a click lands on the rows
  where the player saw them: a rejection's line under a listening row goes
  when its listening ends.
- While a binding listens, `SettingsWindow` returns true from
  `Window::CapturesKeys`, and `InputCapture` hands each key pressed to
  `Window::OnCapturedKey` ([Input focus](#input-focus)). The window keeps the
  first key until the overlay's next frame takes it, in an atomic, since
  the input thread writes it.

### Connection controls

`[Connection]` is one step: the name peers see, and joining with it. The
rows below hold `ConnectAutomatically`, `CustomServer`, `ServerAddress`, and
`ServerTls`, so every setting that enables or disables the button is on the
same page. `SettingsWindow` draws `[Connection] DisplayName` as a label over
its text field and the connection button, as multiplayer clients lay out
joining a server, and `ConnectionControls` draws the button (`DrawButton`)
and one line under the two (`DrawNotice`), then a divider
(`DrawConnectionDivider`). The section has no status, server name, or ping of
its own: the sidebar's summary and the status panel show those.

- The field applies like any text setting, when its editing ends. Applying it
  by a click elsewhere never connects.
- The field takes only the characters a display name may hold
  (`Multiplayer::IsDisplayNameCharacter`) and at most
  `Multiplayer::kMaximumDisplayNameLength` of them (`FilterDisplayName`):
  other typed or pasted characters are dropped, and a paste past the limit
  is cut at it. Text shorter than `kMinimumDisplayNameLength` is still
  invalid, so the field can be invalid while a name is being typed.
- Enter or keypad Enter in the field applies its text and, when the button
  could connect (`ConnectionControls::CanConnect`), connects like a press of
  the button; otherwise it only applies. The window stays open. The editor
  only records the request, and the window connects after
  `ApplyLiveSections`, so the hello never carries the display name from
  before the edit ([Network](network.md#sessions)).
- The field is editable only while `Network::GetConnectionState` is
  `kDisconnected`, because a session takes its display name from the hello
  ([Network](network.md#sessions)). Otherwise it is disabled, and hovering it
  shows why (`ConnectionControls::GetDisplayNameLock`): the connection must
  be cancelled while connecting, and ended while connected or disconnecting.
- Opening the window with no display name applied puts the keyboard in the
  field. Escape then leaves the field, and a second press closes the window.

The button and Enter in the field are the only controls that connect. The
button follows `Network::GetConnectionState`, so it never claims a state the
transport has not reached, and keeps its width and identity when its label
changes:

| State | Button | Press |
| --- | --- | --- |
| Connected | disconnect, `DangerButton` | stops wanting a connection |
| Connecting | cancel | stops wanting a connection |
| Disconnecting | disabled | — |
| Disconnected, the field holds a display name | connect, `PrimaryButton` | wants a connection |
| Disconnected, the field is empty or invalid, `CustomServer` is on without an address, or the [vanilla intro](network.md#vanilla-intro-hold) holds the connection | connect, disabled | — |

Connecting goes by the field's text, not by the applied display name:
pressing the button ends the field's editing, which applies the text before
the press lands, so a name that was only typed connects with one click.

While disconnected, hovering the disabled button shows why it cannot connect
(`ConnectionControls::GetButtonLock`): that the player cannot connect at this
point of the game while the vanilla intro holds the connection
(`kConnectionGameStage`), or, with `CustomServer` on and no address, that an
address is needed. These are the only places the plugin shows either. Enter
in the field does not connect in either case (`CanConnect`).

The line under the field and the button shows the first of these that holds,
and is the only place the plugin shows them. Otherwise it keeps its height
empty, so the rows below do not move:

| Condition | Line |
| --- | --- |
| The field's text is not a display name ([Network](network.md#sessions)); the filter leaves being too short as the only way | the minimum length, as an error |
| Disconnected because the server refused the connection (`kDisplayNameTaken`, `kDisplayNameInvalid`, `kClientOutdated`, `kServerOutdated`, `kSessionReplaced`, `kServerFull`, `kTooManyNewSessions`, `kAddressFull`, `kBanned`) | the reason of `Network::GetConnectionRefusal` (`FormatRefusal`), as an error, until the button connects again. `kClientOutdated` and `kServerOutdated` tell whether the server runs a newer or older version of the mod, without protocol numbers |
| Disconnected by a kick (`kKicked`) | nothing; the [notice](#notices) told the player |

The sidebar's summary (`ConnectionControls::DrawSummary`) is a state
indicator (`Theme::kPositive` connected, `kWarning` pulsing while connecting,
`kNegative` refused, neutral otherwise) and the state on one line, with the
ping while connected.

A game that starts with `ConnectAutomatically` and a display name connects by
itself; nothing else connects without a press. A refusal leaves the player
disconnected with the field editable, and the button then retries with
whatever is applied, which clears the line. The refusals are in
[Network](network.md#connection).

Not wanting a connection closes it and removes every remote player until the
next connect. The button acts on the live settings, which a display name or
address reaches when its editing ends. Applying an address or switching
`CustomServer` or `ServerTls` keeps a live connection on its server; the next
connect, or a reconnect after a loss, goes to the server they now name
([Network](network.md#connection)).

### Discord link

The bottom of the `[Connection]` page holds an invitation to the mod's
Discord server (`DiscordLink`): the [logo](#logo) font's Discord symbol and a
one-line invitation (`SettingsDiscord`), as one bordered button; a click
opens the invite in the default browser. Without the font, the text alone is
in the border.

- The invite comes from the build: the `discord_invite` xmake option
  (`xmake f --discord_invite=https://...`, default empty), which the
  PlayerHub target's `on_config` passes as the `PLAYERHUB_DISCORD_INVITE`
  string define, like `server_address`
  ([Configuration](configuration.md#connection)). A build without it has no
  link at all (`DiscordLink::IsEnabled`). The define is present even when
  empty, because xmake 3.0.6 recompiles for a changed define but not for one
  that was added. A value that does not start with `https://`, or that holds
  whitespace, a quote, or a backslash, fails the configuration.
- `EndSection` puts it at the bottom right of the section's scrolling area,
  by `DiscordLink::GetSize`, as low as the area's closing spacer still fits
  under it, so it never makes the page scroll; when the rows reach lower, it
  follows them.
- `ShellExecuteW` runs on a detached thread with COM initialized, because it
  waits for the browser to take the URL; a failure logs a warning with its
  result.

## Item picker

`UI::ItemPicker::Draw(id, value, default, filter, allowNone)` draws one form field as a searchable
dropdown of loaded items that fit the setting's filter, instead of typing
`Plugin|FormID`. A window keeps one picker for all its fields: it caches each
field's label until the value changes, and only one list can be open. The
catalog and filters are in
[Appearance and equipment](appearance-and-equipment.md#item-filter-and-catalog).

- The picker has no colors or sizes of its own: it takes the style of the
  window that draws it, the [theme](#theme) in the settings window, and
  `Theme::kWarning` for warnings.
- The closed field shows the item's name, its `Plugin|0xID` marked as not
  loaded for a missing plugin, or the name marked as a warning for an item
  the filter rejects, each with an explaining tooltip. A value for a plugin that is not loaded can
  still be written to the file.
- The open list has a search field that takes keyboard focus and a
  two-column table of name and plugin. A setting that may be empty
  (`allowNone`, from the `Config::SettingsVisitor::Form` call) has a none row
  first while the search field is empty, which picks the empty reference and
  shows none in the closed field; other settings have no empty
  row, since they always hold an item. Only visible rows are drawn
  (`ImGuiListClipper`), and the current value is scrolled into view.
- Clicking a row picks it, Enter picks the first row, Escape closes the list.
- While the value differs from the setting's default, the field narrows for a
  reset icon button beside it, which sets the default; its tooltip names the
  default item.
- With `[UI] ItemPreview`, a square panel to the right shows the
  hovered item's model, or the current value's, with its name and
  `Plugin|0xID` below; it is empty for the none row or an empty value. Race lists have no panel, since the inventory renderer
  shows only items (`Forms::HasModel`).

### 3D preview

`UI::ItemPreview` renders one item at a time through the game's own inventory
renderer: as the model of one panel (the trade window's
[preview column](#preview-column) and the item
picker's panel) or as cached thumbnails for many cells.

- `DrawModel(formID, size)` takes the area as an invisible button and draws
  the item's model fitted and centered in it over whatever is behind it; the
  item picker draws its panel first. A new item shows nothing for a
  moment, then a loading state, and a no-preview state once it fails. While
  the button is held, the mouse turns the model, the horizontal part negated
  as the inventory menu's mouse handler does. A new item starts unturned.
- Once captured, the model spins about its vertical axis at
  `kSpinRadiansPerSecond`. Holding the button pauses the spin, which resumes
  from where the player left the model when it is released. A spinning model is captured at most every `kSpinCaptureIntervalSeconds`
  (1/30 s), on the update closest to it assuming the next update is as long
  as the current one, since each capture renders the inventory scene twice
  on the main thread. The draw
  keeps the last capture in between. A turn, a new size, a watch, or an
  animation still captures on its own schedule. The spin advances by the
  frame time of the updates that render the model or wait for its next
  spin capture (`Model::spinWaited`), so it pauses rather than jumps while
  thumbnails hold the renderer. A light (`FormType::Light`) does not spin,
  since ID 51778 does not turn it.
- `Update` runs in `Overlay::DrawFrame` before `ImGui::NewFrame`, when the
  game frame is finished and the overlay has not drawn. It advances a clock
  by the clamped frame time and picks one form to have loaded
  (`ChooseWork`): the model's when it is urgent (its first capture, a turn,
  or a new size), else a still waiting for its first capture (the loaded
  form's, else the earliest requested), else the model's for its refresh
  or while a fresh load is watched, else a still due for its refresh, else
  an [animated](#3d-preview) model's or one whose spin capture is due, else
  nothing while a spin capture is not yet due, which keeps the renderer and
  its load, else none, which stops the renderer (`End3D`) and keeps the
  images. Pending stills come before
  the model's refresh and glow watch, which would otherwise hold stills back
  for seconds while the player moves between items.
  `Inventory3DManager::Begin3D(kInventory)` is called once, and
  `LoadInventoryItem` only when the form changes or a refresh needs a load
  that started after the capture it replaces.
- A load serves its whole form (`ServeLoaded`): once it is ready, the same
  update captures every still of the form that a window drew last frame
  and that is due, then the model if it is the form's and due, animated,
  or due a spin capture. So adding the hovered item to an offer captures its cell's
  still from the model's load on the next update, without loading again.
  A model loading in the background is
  attached only by `Render`, so while none is attached, `Update` calls
  `Render` with `UI::numDisablePauseMenu` at zero, which draws nothing. A
  model counts as attached only when the last `loadedModels` entry's
  `itemBase` is the loaded form, since the previous item's model can still be
  attached a frame after a new load. A job captures once the model is
  attached, one more update pass has run, and every diffuse and normal
  texture of its lighting materials has a `rendererTexture` with a D3D
  texture and view; a model that has not rendered within
  `kCaptureTimeoutSeconds` (5) of frame time fails and logs why, and one
  whose textures are still missing then is captured as it is. The timeout
  counts only updates on which the load was not ready or a capture failed,
  and it fails every request of the form the load was serving.
- A capture projects the model's world bound through
  `UI3DSceneManager::viewFrustum` to a screen rectangle, saves that
  rectangle of the frame target, clears it
  (`ID3D11DeviceContext1::ClearView`), calls `Render` with the count at least
  one, copies the rectangle out, and puts the saved pixels back.
- The model is rendered twice, cleared to black and to white, so its
  coverage can be told from the frame target's alpha, which is not coverage.
  A `Separate` pass ([Gpu](#backdrop) helpers) takes alpha as one minus the
  mean difference of the two and keeps the color over black, which is
  already multiplied by it, in a 2048² texture drawn with a premultiplied
  blend (`Gpu::Blend::kPremultiplied`), so the model floats with
  anti-aliased edges over the backdrop or the picker's panel.
- The model is captured at twice the pixels it is drawn at
  (`kSupersample`), not at the size the inventory scene gives it: before
  each capture `FitModel` scales it about its bound's center so its square
  capture (the bound's radius projected at the center's depth, plus 5%) is
  twice the smaller side of the area `DrawModel` was given, at most the
  screen and 2048 px, and moves it across the view only as far as keeps
  that square on the screen. A change within 1% is skipped. The area's
  size changing captures it again. The image is drawn at exactly half size
  on whole pixels, so ImGui's bilinear sampler averages each 2 × 2 texel
  block, a box filter that anti-aliases the edges the capture has without
  the game's TAA (below); a capture the screen clamped is filtered into the
  area as it fits.
- Every capture renders with the game's TAA off. In 1.7.104 the UI 3D scene
  render (ID 52727) checks `ImageSpaceManager`'s `ISTemporalAA` block
  (`+0x1F0`, `taaEnabled` at `+0x18`): when it is on, the render jitters the
  camera, adds the motion vector target, and resolves the item through
  temporal AA, which converges over the inventory menu's frames but leaves
  a single capture soft. A scope clears the flag for the capture's
  `Render` calls and restores it; the first capture of each start logs
  whether it was on (`gameTAA`). The 2× capture above is the model's
  anti-aliasing instead; thumbnails get theirs from the mip reduction.
- Every capture renders with its own directional ambient, so an item looks the
  same wherever and whenever it is captured, and faces turned from the scene's
  light are not black. In 1.7.104 the inventory scene sets none, so the
  lighting shader takes the world's from `BSShaderManager::State`, which ID
  26232 sets from the current cell's lighting or the weather and time of day.
  `CaptureAmbientScope` does what MistMenu's render (ID 52308) does around its
  scene render: it calls ID 105643 (SE 98989) with the same color
  (`kCaptureAmbient`, 0.45) for all six directions, MistMenu's white ambient
  specular tint, and a fresnel power of zero, then puts back the state's
  `directionalAmbientTransform`, `ambientSpecular`, and
  `ambientSpecularEnabled` and the colors ID 105643 keeps (ID 414646, SE
  527705, which MistMenu reads back through ID 105644). The scene's own light,
  the `fInventoryMenuLight*` INI point light in front of and above the item,
  stays as the inventory menu has
  it and gives the shading. The first capture of each start logs the world's
  ambient it replaced (`worldAmbient`, the transform's translation).
- After restoring or fitting a transform, `ItemPreview` calls
  `NiAVObject::Update` (ID 70251) with a time of zero and the dirty flag,
  as `Render` does before drawing, so the world bound it projects and turns
  about is current.
- The model is captured again `kRefreshDelaySeconds` (1.5) after its first
  capture from a fresh load and keeps its image if the refresh fails. This
  covers textures that complete after the first capture by a path the
  readiness check does not see.
- A turn calls the inventory menu's own rotation (1.7.104 ID 51778, SE 50902)
  with the angles dragged since the last capture, then captures, so a drag
  re-renders every frame while the renderer holds the model. The model's local
  transform as fitted and turned is kept after each capture and set again
  before the next, also after stills took the renderer, so it neither snaps
  back nor is fitted twice.
- Loads share models. In 1.7.104 `LoadInventoryItem` (ID 51758) reuses a
  model still in `loadedModels` (up to 7, keyed by model object) as it was
  left, and only `End3D` (ID 51756) releases them, so a still captured
  after the panel showed the same item would get the panel's size and turn.
  The first time a form attaches after `Begin3D`,
  `ItemPreview` keeps its local transform as the game's (`gameTransforms`,
  cleared on stop). Each still capture starts from it, and each model
  capture from the model's kept transform (`SetTransform`), since both
  share one load.
- A model is animated when a node, geometry,
  or geometry property in its scene graph has a time controller, or a
  shader property has effect data (`BSShaderProperty::effectData`, which
  is where an enchantment's glow is). The check runs after each capture
  until one finds either. The game attaches the glow a frame or two after
  the model, so a form with its own enchantment
  (`TESEnchantableForm::formEnchanting`; items load without extra data, so
  no other can glow) is therefore watched: after the first capture of each of its loads, the
  refresh's included, the model is captured on every update for
  `kEffectWatchSeconds` (0.5) while no still waits for its first capture,
  so it shows its glow without being turned. A still, captured
  once per load, instead waits for effect data until
  `kStillEffectWaitSeconds` (0.25) have passed since its load attached or
  in all across its loads (`ThumbnailSlot::effectWaited`, so a model
  taking the renderer back does not restart it), running only `Render`'s
  update pass with the count at zero meanwhile; one without a glow by then
  is captured as it is: the glow comes within milliseconds, and some
  enchanted items animated by a time controller never get effect data, so a
  longer wait from each load would hold back every still after it. An
  animated model is captured on every update that has no other job, as the
  inventory menu renders it each frame, since captured only when due its glow
  would freeze between captures and flicker during a drag. Afterwards it
  gives way only to a thumbnail that is due, so the loaded
  item does not switch every frame. The first capture that finds it
  animated logs the counts (`Item model animated`), and the first 6 log
  the first effect data's offsets and alphas against the clock
  (`Item model effect`).
- When no window drew a model or a thumbnail in the previous frame,
  `Update` unloads the item, calls `End3D`, and frees the textures and the
  thumbnails; when only the model was not drawn, it forgets the model. If
  an item menu opened meanwhile (`UI::IsItemMenuOpen`), that menu owns the
  renderer, so the state is dropped without calling it.
- The preview turns itself off for the session with one warning when the
  overlay does not run on the bound main-loop thread
  (`GameThread::IsAuthoritative`), the inventory renderer is missing, or
  capture textures cannot be created for the back buffer format. The model
  alone turns off when its textures or its shader cannot be created.
- The game is not paused. In 1.7.104 `Render` attaches a
  finished load (load task state 5), updates the model, and tail-calls the UI 3D
  scene render (ID 52727), which draws nothing unless the scene's light scheme
  matches and `UI::numDisablePauseMenu` (`UI + 0x168`) is positive. The count is
  positive while an inventory menu is open. The overlay is not a menu, and
  opening one would take input focus, so `ItemPreview` sets the count for the
  call and restores it. The scene draws into the engine's current render
  target, the back buffer at the overlay hook.
- `DrawThumbnail(drawList, formID, background, min, max)` fills the rectangle
  with the opaque `background` and draws the item's still fitted and
  centered, returning `kDrawn`, `kLoading` while it waits for its capture,
  or `kUnavailable` (thumbnails off, unavailable, or the form failed), when
  the caller draws something else. A still is kept per form and background
  in one of 64 slots of 256² in a 2048² atlas; a new one takes a free slot,
  else the one drawn longest ago that no window drew this frame.
- A still is due when a window drew it last frame and it waits for its
  first capture or its refresh. Its capture logs `Item thumbnail captured`
  with the seconds since it was requested, since its load started, and
  since the load attached (`sinceRequest`, `sinceLoad`, `sinceAttach`),
  and whether the glow was seen (`effect`). `FitModel` sizes it to a 512 px square capture
  (`kThumbnailCaptureSide`, twice a slot) and its box is projected at the
  bound center's depth, as the model's is. It
  is captured on its background into a 2048² scratch texture with a full mip
  chain. `GenerateMips` halves it until it fits a slot, and the level is
  copied in, dropping the partial last texel each level adds. Each still is
  captured once more, after a fresh load, when 1.5 s of frame time have
  passed since its first capture and no other thumbnail in view waits for
  its first; the cell keeps the first still meanwhile, and keeps it if the
  refresh fails.
- A still is drawn with blending off (a draw callback sets an opaque blend
  state, then `ImDrawCallback_ResetRenderState`), since the back buffer's
  alpha under the model is not coverage; captured on its cell's background,
  it shows no seam.
- Thumbnails turn off for the session with one warning when the back buffer
  format cannot generate mipmaps (`D3D11_FORMAT_SUPPORT_MIP_AUTOGEN`) or
  their textures cannot be created; cells then show names.
- Logs: `Item preview started` and `stopped` (debug), the first capture of
  each start with its screen box, bound radius, and the game's TAA and
  world ambient it replaced, and `Item preview
  unavailable: <reason>` or `Item model preview unavailable: <reason>` once.
  A model waiting a second after its load logs `Item preview waiting` once
  (debug) with the models `Inventory3DManager` holds, the last one's form,
  its load task state, and both light schemes; its first and refresh
  captures log `Item model captured` (debug) with the size and the textures
  checked and still missing, and a timeout `Item model: form <id> did not
  render within 5 s`. Each thumbnail capture logs `Item thumbnail captured`
  (debug) with its box, mip level, whether it is the refresh, the textures
  checked and still missing, and the first diffuse texture's path. A model
  waiting for textures logs `Item preview waiting for textures` once
  (debug) and, for a thumbnail, when captured, `Item thumbnail: form <id>
  captured after <s> s waiting for textures`; a thumbnail's timeout logs
  `Item thumbnail: form <id> did not render within 5 s` with the wait state,
  and `Item thumbnails unavailable: <reason>` once.

## Localization

The overlay's text and the notices are in English, compiled into the plugin,
or in a translation read at startup from a gettext PO file in
`PlayerHub/locale/` beside the DLL. The distribution ships the game's official
languages there: French, German, Italian, Spanish, Polish, Russian, Japanese,
and Traditional Chinese. Anyone can add a language by putting another
`<code>.po` there and restarting ([Translation files](#translation-files)).
Until the player picks one with `[UI] Language`
([Configuration](configuration.md#ui)), the language is the game's: the
translation whose `X-Skyrim-Language` header matches `sLanguage:General` from
the game's INI, the setting that also picks its string tables, or English when
none does.

- The `[UI] Language` combo, the first row of the `[UI]` page, lists English
  and then every loaded translation (`Text::GetLanguages`), each by its own
  name, so a player can find theirs whatever language the text is in. The
  name is the translation's `LanguageName` text, or its code when it leaves
  that untranslated. While the setting is empty, or names no loaded
  translation, the combo shows the game's language selected
  ([Settings window](#settings-window)). Picking a language stores its code.
  A picked language stays, even if the game's changes, until the page's
  Defaults button empties it again.
- The overlay applies the setting before `Fonts::Load` and then in every
  frame where `Config::GetRevision` has changed, before `ImGui::NewFrame`:
  `Text::Select` with the setting's code. An empty code selects the game's
  language. So does a code with no loaded file, which logs
  `Localization: no <code>.po in PlayerHub/locale; using the game's language`
  once until the setting names another code.
  The new language shows from that frame without a restart. When it puts
  another Han font first ([Fonts](#fonts)), `Fonts::Update`, between frames,
  calls `ImFontAtlas::Clear`, unmaps the old files, and loads the fonts
  again in the new order. `Clear` rather than `ClearFonts`: after
  `ClearFonts`, ImGui packs the new glyphs into the same texture without
  erasing it, and the DX11 backend uploads only each new glyph's rectangle,
  so pixels of the old glyphs beside them show at their edges. `Clear`
  starts a new texture, uploaded whole, and retires the old one. A switch
  that keeps the Han order keeps the fonts.
- Text drawn each frame follows at once. The connection button's labels, the
  short-name line, the item pickers' labels, the chat's target chip, and
  [nameplates](#nameplates) (whose label is localized without a name) are
  cached with the language they were made in and made again after a switch. Text already composed
  keeps the language it was made in: chat lines and notes, notices, trade
  rejection reasons, and the item card's unknown effect line.
- Every displayed text has a `Text::Id`. Its text comes from `Text::Get`,
  `Format`, or `FormatPlural`. English is complete and is the fallback for any
  ID a translation leaves untranslated, marks fuzzy, or has rejected.
- `Text::Initialize` runs once, on the overlay's first frame before the fonts
  load, and again when the game's data loads in case the overlay never
  started; `std::call_once` makes the later call do nothing. It loads every
  translation file, reads the game's language, keeps it for
  `Text::GetGameLanguage`, and selects it, logging
  `Localization: the game's language <sLanguage> is <code>`. Before that, text
  is English. Each language's table is built at load and kept for the session.
  Selecting publishes a table through an atomic pointer, so `Get` from any
  thread is an index into the current table, and a thread still reading the
  previous one reads valid text. `Text::Language` is an index into
  `Text::GetLanguages`, English first, which is what the caches above compare.
  The list is built on the main thread and only read there. A switch is
  logged as `Localization: switched to <code>`. Initialize also logs the
  English table's own errors.
- A text is a `std::format` string. Each translated text is checked against
  the English one when its file loads: it may use only fields the English
  has, with the same index and format spec, and a text that is not a plural
  form must use all of them. Mixed automatic and numbered fields, a stray
  brace, or a nested field reject it. So formatting cannot throw, and a
  translation can reorder its arguments with `{0}` and `{1}`.
- A plural text has as many forms as its file's `Plural-Forms` header gives,
  up to `kMaximumPluralForms` (6): one for Japanese and Chinese, two for
  English, French, German, Italian, and Spanish, three for Polish and Russian
  (one, few, many). `Text::GetPlural` picks the form with the rule of the file
  the text came from, a `Text::Gettext::PluralRule` that evaluates the
  header's C expression, and `FormatPlural` passes the count as argument 0. A
  form may leave out a field, but a translation with the wrong number of forms
  is rejected.
- A sentence that draws an argument in the accent color, such as the
  [display name hint](#display-name-hint), the trade window's title, and the
  [trade prompt](#prompt), is a format whose fields `Text::Split` cuts out.
  `Controls::AccentedText` and the prompt's centered line draw the literal
  pieces with their own spaces and the arguments in between, so a translation
  places the name or key where its word order needs.
- What stays English: the INI file's comments and header, so the file is the
  same whatever the language ([Schema](configuration.md#schema)); config keys
  and values; wire values; log lines; and the `@` of a mention. Chat lines are shown
  as they were sent. Item, race, and effect names come from the game's own
  data and so are in its language already, as are the tempering tiers in
  [trade](trade.md#item-details) item names, which come from the
  `sHealthDataPrefixWeap1`–`6` and `sHealthDataPrefixArmo1`–`6` settings.
- The [item card](#preview-column) lets lines break between Japanese and
  Chinese characters, which have no spaces between words. It does not break
  before closing punctuation and small kana, or after opening brackets.
- Controls sized for English text fit longer text instead: the settings and
  trade footers' buttons and the connection button take the widest of their
  labels.

### Translation files

The texts are gettext PO files. Gettext was chosen so standard tools
(`msgmerge`, `msgfmt -c`, PO editors) work on them. The translations are read
at startup rather than compiled in, so a community translator can make, fix,
and share a language without a plugin build.

- `locale/playerhub.pot` is the English source and the list of IDs. Each entry's
  `msgctxt` is its `Text::Id` without the `k` prefix, `msgid` its English text,
  and `msgid_plural` the plural form, if any. A `#.` comment tells translators
  what each field is. An entry with fields needs the `c++-format` flag, so
  `msgfmt -c` checks the translations' fields. The header's `Plural-Forms` is
  English's rule. The `LanguageName` entry's English is `English`; each
  translation gives its language's name in that language.
- A translation is `<code>.po`, and the file name is its code, which
  `[UI] Language` stores. Its `Language` header is the same code. Its
  `X-Skyrim-Language` header, for a language the game has, names the
  `sLanguage:General` value that selects it while the setting is empty. The
  shipped files in `locale/` are `fr`, `de`, `it`, `es`, `pl`, `ru`, `ja`, and
  `zh_TW` (the game's Chinese is Traditional). An empty `msgstr` leaves the
  text in English.
- The build's `after_build` step copies `locale/*.po` and the template, for
  translators, to `PlayerHub/locale/` in each install directory.
- `Text::Initialize` lists the `.po` files (extension in any case) in
  `PlayerHub/locale/` under `Config::GetDirectory`, in file name order, and
  loads each into a table. It skips a file, logging
  `Localization: skipping <file>: <reason>`, when its name is not a language
  code (`Text::IsLanguageCode`: a letter, then letters, digits, `_`, `@`, or
  `-`, at most 32 characters) or is `en`; when another file has the same code
  in any case; past 254 translations, since `Text::Language` is a byte; over
  4 MiB; when it does not parse; or when its `Plural-Forms` is missing, has
  more than 6 forms, or gives a form out of range or divides by zero for any
  count from 0 to 9999.
- Within a file, `msgctxt` matches an entry to its `Text::Id`. An entry with
  no such ID, or one listed twice, is skipped with a warning. A fuzzy or
  untranslated entry is not used, as gettext does not use one. An entry that
  translates another English text than the template's (a different `msgid`
  or `msgid_plural`), has the wrong number of forms, or has fields that differ
  from English (see above) is rejected with a warning naming its file and
  line. Each file then logs `Localization: <file> with <n> of <total> texts
  translated (<f> fuzzy, <r> rejected)`, followed by ` for <sLanguage>` when
  it has `X-Skyrim-Language`.
- `Text::Gettext::Parse` reads what `msgmerge` and PO editors write:
  `msgctxt`, `msgid`, `msgid_plural`, `msgstr` and `msgstr[N]`, continuation
  lines, comments, and the `fuzzy` flag. It skips obsolete `#~` entries and
  accepts a UTF-8 byte order mark and CRLF line endings. It knows the escapes
  `\n`, `\t`, `\r`, `\\`, and `\"`; any other fails the file, with its line.
- The `locale` rule (`locale/xmake.lua`, added to the plugin target) runs
  `generate.lua` in xmake's prepare stage, before anything compiles. Its output
  goes to `rules/locale/text/locale/` in the target's autogen directory, which
  is on the include path, so code includes `text/locale/TextId.h`. The rule
  adds the generated `English.cpp` to the target at load with `always_added`,
  as it does not exist until the prepare stage. A file is written only when
  its content changes, so editing a translation recompiles nothing, and only
  a change to the template's IDs recompiles the users of `TextId.h`.
  Nothing generated is committed, and the generator is xmake Lua because the
  Windows build runs under wine without Python.
- The output is `TextId.h` (`Text::Id` in template order, then `kCount`),
  `English.h`, and `English.cpp` with `Text::Catalogs::GetEnglish`: each
  entry's ID, key, text, and plural form, and the template's `Plural-Forms`,
  which `Text.cpp` parses at startup as it does a translation's.
- The build also checks every `locale/*.po` as the plugin does when it loads
  one, so the shipped files load whole. It fails, naming the file and line, on
  a key not in the template or listed twice, a translation of an older
  English text, an entry marked `fuzzy`, a missing plural form, an invalid
  format, or fields that differ from English. It also fails on a file name
  that is not a code or is `en`, a `Language` header other than the file's
  code, an `X-Skyrim-Language` not in capitals, and a `Plural-Forms` the
  plugin would skip. It warns of a file that translates only some texts.
  `xmake l locale/generate.lua [outdir]` runs the same checks without a build
  and prints each translation's count.
- To change a text, edit the template, run `msgmerge -U <code>.po
  playerhub.pot` for each translation (it marks entries whose English changed
  as `fuzzy`), update the translations, and build. A community translation
  updates the same way from the shipped template. Until it does, the texts
  whose English changed show in English.
- To add a language, make `<code>.po` from the template (`msginit`, or a PO
  editor), set its `Language`, `Plural-Forms`, and, if the game has the
  language, `X-Skyrim-Language`, translate it, and put it in
  `PlayerHub/locale/`, or in `locale/` to ship it with the plugin.

## Engine assumptions

Not confirmed in game. IDs are 1.7.104's, read from its disassembly, with SE
IDs matched against 1.5.97 where given.

1. A model captured again `kRefreshDelaySeconds` after its first capture,
   from a fresh load, is drawn with all its textures. Some first captures,
   taken one update pass after attaching, lack textures even though every
   lighting texture already has its `rendererTexture`:
   `BSShaderManager::GetTexture` (ID 105640) reads a texture file
   synchronously on the model loader thread, and `BSShaderResourceManager`
   (vtable slot `0xD0`, ID 108531) sets `rendererTexture` when it creates the
   `NiSourceTexture`. What the first capture misses is not identified.
2. Skyrim's back buffer format supports `D3D11_FORMAT_SUPPORT_MIP_AUTOGEN`,
   as `R8G8B8A8_UNORM` does, and `ClearView` writes the same value the
   overlay's draw of the same 8-bit color does, so a cell's background meets
   its thumbnail's without a seam.
3. Starting and stopping the inventory renderer (`Begin3D`, `End3D`) for
   each batch of thumbnails costs no visible frame time.
4. `LoadedInventoryModel::itemBase` of the model `LoadInventoryItem`
   attaches is the form passed to it. A reused entry is found by its model
   object (ID 51771) and keeps its `itemBase`, so two forms sharing one
   model object within a run would leave the second waiting until its
   capture timeout.
5. The UI 3D scene draws a model the same over black and over white within
   one frame, so the difference of the two captures is its coverage: no
   part of the scene render reads the cleared background.
6. ID 51778, called as the inventory menu's mouse handler (ID 51770) calls
   it, with a `NiPoint2` of angles in radians, turns the last loaded model
   about its bound's center by changing its local transform. `Render` does
   not reset that transform: its zoom step (ID 51777) moves the model only
   while a zoom is under way. Setting the kept transform on a newly attached
   model before its update pass shows it turned.
7. The state `PassScope` puts back is all the engine's state tracking
   relies on, so the frame after a blur or a model capture draws unchanged.
8. The frame target at the overlay hook is single-sampled and can be copied
   whole (`CopyResource`) into a texture of its own format.
9. Scaling a model's local transform in the inventory scene keeps its
   lighting and textures as the unscaled model has them, and calling
   `NiAVObject::Update` (ID 70251) with a time of zero on the attached model
   advances nothing visible: a controller accumulates the time since its last
   update (`NiTimeController::ComputeScaledTime`, ID 70824), so the zero
   steps it back by the last time and the update `Render` makes before
   drawing, at the application's run time (ID 410201, in ms), steps it
   forward again.
10. Clearing `taaEnabled` for the scene render's call removes the jitter,
    the motion vector target, and the TAA resolve together (the camera setup
    it calls, ID 77506, takes the same flag), and nothing else that frame
    reads the flag between the clear and the restore.
11. An enchanted item's glow, effect data on a shader property, is attached
    by the game within `kEffectWatchSeconds` of the model's attach; what
    attaches it is not traced. Only forms with their own enchantment get one,
    and `Render` with the count at zero, which draws nothing, still lets it
    attach.
12. The lighting shader reads the directional ambient and ambient specular
    from `BSShaderManager::State` during the scene render's call, so setting
    them with ID 105643 for the call and restoring the fields afterwards
    lights the capture without changing the world's frame: ID 105643 writes
    only those fields and its color copy at ID 414646, the inventory scene's
    render path does not call it, and the lighting shader's geometry setup
    (ID 107312) reads the transform.
13. [Input language](#input-language) switching works for every layout as
    for English and Russian. Dead keys compose in the polling thread's own
    keyboard state, so a dead key pressed while the poll changes threads may
    be lost.
14. Nameplates: `Actor::HasLineOfSight` (ID 53829) on the player calls ID
    40520, which sets its second argument from a frustum test of the target's
    bound and only then casts a ray, on collision layer `0x29`, from the
    camera. Other remote actors and the target's own collision do not block
    the ray. `WorldRootCamera`'s `worldToCam` at the overlay hook is the
    matrix the frame was rendered with, its fourth row gives a depth that is
    positive in front of the camera, and its viewport is 0 to 1 with y up
    (ID 70640's arithmetic matches a standard projection). SE IDs: 53029,
    69270, and 39444 for ID 40520, with the same layer.
15. `INISettingCollection` holds `sLanguage:General` with the game's
    language by the overlay's first frame, since the renderer starts after
    the INI files are read. Its values for the official languages are
    `ENGLISH`, `FRENCH`, `GERMAN`, `ITALIAN`, `SPANISH`, `POLISH`, `RUSSIAN`,
    `JAPANESE`, and `CHINESE`, matched without case, and `CHINESE` is the
    game's Traditional Chinese.
16. The `sHealthDataPrefixWeap1`–`6` and `sHealthDataPrefixArmo1`–`6` game
    settings hold the tempering tiers' names in the game's language. An empty
    or missing one falls back to the English name.
17. Without a plugin that redirects it, the `kFRAMEBUFFER` render target
    view at the overlay hook views the swap chain's back buffer, and the
    menus have drawn on it. Community Shaders draws its own ImGui menu there
    at Present when its HDR output is off. Under its HDR output, the
    inventory renderer used by item captures draws on the UI texture that
    `kFRAMEBUFFER` then names. Its frame generation proxy's swap chain buffer
    has both shared flags, and a real swap chain's buffer has neither (read
    from its source).
18. `BSTEventSource<InputEvent*>::SendEvent` calls its sinks with an empty
    list as with any other, and stops at the first sink that returns
    `kStop`. Community Shaders and SKSE Menu Framework dispatch an empty list
    while their menu is open (read from their source), also on 1.5.97.
19. The game's cursor height follows from `CursorMenu`'s constructor loading
    its movie with `ScaleModeType::kShowAll` (the `mov dword ptr [rsp+0x20],
    1` before `BSScaleformManager::LoadMovie`), the movie's 640 × 480 stage,
    and the shape's placement, assuming the movie's viewport is the whole
    screen, so the stage fits the screen's height.

## Acceptance criteria

1. No status panel shows in the main menu or, by default, in a game. With
   `StatusPanel` on, in a game the panel shows the connection, ping,
   traffic, and plugin time; turning it off hides it at once.
2. `OpenChat` opens the chat only with no menu open; the character stands
   still while typing, and a key held from before keeps acting until
   released. Enter sends, the line appears for both players, and the chat
   closes, or with `CloseChatOnSend` off stays open, where Enter in an empty
   field does nothing. Escape closes without sending and does not open the
   journal; a menu that opens by itself closes the chat. Recent lines show
   over the game without a background, readable over bright scenes, and fade
   after 10 s. Sending while disconnected shows a failure note and keeps the
   text; typing clears the note.
3. The chip starts at Nearby, and a line reaches players within
   `chat.nearby_radius` or in the same interior, not farther ones. Tab turns
   the chip to Global, which reaches every player until Tab again. `/g hi` is
   sent as written.
4. `OpenSettings` opens the settings window with a cursor, the game receives
   no input until it closes, and the key closes it again; a rebound key works
   at once and the old one no longer does. With another plugin's ImGui menu
   bound to Delete, Delete and letters typed into the chat or a settings field
   do not open its menu, and Delete opens it once the window is closed; the
   log has one line about detouring input dispatch again. With the Community
   Shaders or SKSE Menu Framework menu open, focus keys open no overlay window
   and reach that menu; once it closes they work again.
5. With no display name, opening the window puts the keyboard in the
   `[Connection]` field and the button cannot connect. Typing a name and then
   one click on the button, or Enter, connects with the window still open; a
   click elsewhere only saves the name. Characters a display name cannot hold
   cannot be typed or pasted, nor more than the maximum. Connected, the button
   disconnects and the field is disabled with a hover hint to disconnect
   first; disconnecting removes remote players and keeps them away through a
   save load until the next connect. While connecting the button cancels and
   never claims a connection. A taken display name shows the refusal under
   the field until another name connects. A name below the minimum length is
   invalid and disables connecting. The sidebar shows the same status in
   every section.
6. Settings apply and save as they are edited, sliders on release, text when
   editing ends, and a typed display name never connects by itself. A slider
   with a zero name shows it at its left end. Each page's Defaults restores
   only its own settings, leaves the chat's size as dragged, and is disabled
   again. `[DefaultOutfit]` applies only on Apply, and its Defaults leaves
   other sections alone.
7. The item picker lists only matching items and races. Hovering rows
   switches the spinning model without flicker on the world behind; nothing
   stays on screen after the list closes; the inventory shows its own models
   normally afterwards; the frame rate stays steady while a list is open;
   `ItemPreview` off removes the panel.
8. With the default accent, everything the [theme](#theme) marks follows the
   accent, and the window can be dragged by its sidebar or title. Picking a
   sidebar item shows that section.
9. The themed windows and the chat are in Sovngarde, headings and line labels
   in the heading face, the status panel in the system font, and the log has
   one `UI overlay: font` line per file. Characters Sovngarde lacks still
   show. With `PlayerHub/fonts/` removed, the log warns for each file and the
   windows use the system font. The sidebar's logo is sharp at every scale,
   its bar follows the accent, and without `PlayerHub.ttf` the log warns and
   a text title shows. A build with `discord_invite` shows the Discord link at
   the bottom right of `[Connection]` without making the page scroll, and a
   click opens the invite; a build without it shows none.
10. The settings window and the open chat have the game's menu frame;
    content never touches a knot, also after resizing and at both ends of
    `BorderThickness`, whose right end matches the game's message box.
    `BorderColor = Accent` turns both frames the accent at once and is
    saved; `Vanilla` turns them back. The log has one
    `UI overlay: menu frame` line; with `PlayerHub/frame/` removed, the log
    warns and both windows have a plain rounded line. Over either window the
    cursor is the [font cursor](#font-cursor), sharp at any resolution and
    scale, clicking at its tip, with tooltips beside it, uncovered; without
    `PlayerHub.ttf` it is the drawn pointer. It becomes a resize cursor on the
    chat's edges and knots, the move cursor while the chat is dragged, and an
    I-beam over a text field. No other cursor shows.
11. A click on another player's name in a line turns the chip to a whisper
    to that player, which only that player receives, labeled as sent to it
    for the sender and as from the sender for the recipient; the next line
    whispers the same player. Once the player disconnects, a note says it is
    offline and the chat keeps the text.
12. With `Ulf` and `Ulfar` online, `hi @u` lists both above the field, and
    `@` alone lists up to 8 others; Tab, Enter, or a click writes `hi @Ulf `
    with the caret at the end, keeping the chip and the chat open. Picking a
    name with the caret inside `@Ulxyz` replaces the whole word. `@ulfar`
    typed in full hides the list and Enter sends; `@zz` shows no list. The
    sent line sets `@Ulf` apart for every player, in the accent for `Ulf`,
    also among recent lines, and still wraps inside the chat; `mail@Ulf` and
    `@Ulfric` with nobody of that name online stay plain. Up recalls a line
    ending in `@Ulf` without listing names.
13. Up recalls the last sent line with its target, and Down returns the text
    that was being written; in a text of several lines they first move the
    caret to the first or last line. Closing and opening the chat clears a
    note.
14. A long text wraps and the field grows upward while the chat keeps its
    size, up to `kMaximumFieldLines`, then scrolls inside. Typing or pasting
    stops at the character limit, in Cyrillic too, and the whole text reaches
    the other player. A pasted line break becomes a space, and Shift+Enter
    sends.
15. Moving the mouse in the open chat shows a cursor, and the character does
    not turn or attack. A right click on another player's line opens its
    player menu; a click on the chip lists Global, Nearby, and the latest
    partners. Typing continues after each click, and a click outside the chat
    changes nothing.
16. The filters are all on at first. A click on one turns only that channel
    off, without changing the send target: its lines leave the open and
    closed chat, and new ones mark it unread. A second click shows the
    channel again with the lines that arrived meanwhile. With Global off, Tab
    to Global turns it on, and so does the player's own global line. With
    all three off no line shows.
17. The open chat has no colors or corners outside the [theme](#theme) and
    `ChatStyle`, and its filters, lines, and chip start at one column, the
    rule and the field ending at one.
18. A wrapped line continues from the left edge under its sender's name.
    Each edge and knot of the open chat resizes it from the opposite edge or
    corner as smoothly as the settings window, within the limits and the
    screen; during and after a left or bottom drag the right or top edge does
    not move. The filters, field, and chip keep their place without
    flickering, and the latest line stays at the bottom. On release the chat
    settles on whole lines and font sizes, which `PlayerHub.ini` keeps across
    restarts, and the position sliders follow a left or bottom drag.
19. The player button shows the number of players online, or that the
    player is offline. A click opens the list beside the chat, nearby players
    first; a click on a name whispers that player and typing continues, and a
    right click opens its menu. The search keeps names containing its text,
    and Escape there closes the chat. After Escape or Enter closes the chat,
    the list shows again when it opens, until the button closes it. The wheel
    over the list scrolls it and not the lines; with the chat at the right
    edge the list is still on screen. With hundreds of players online the
    frame rate with the list open is the same as with it closed.
20. A right click on another player's name in the list or its line offers
    mute and vanish; on the local player's line, neither. After a mute no
    notice shows, the player's lines leave the open and closed chat, earlier
    ones included, its new lines and whispers do not show, and its whispers do
    not move it in the Tab order. After a vanish its character disappears at
    once while the other player still sees this one and is told nothing, and
    it stays gone after either game reconnects or restarts. The list marks the
    player. Unmuting shows every held line again, and unvanishing brings the
    character back if in range.
21. The player list's button opens the player manager over the open chat,
    with friends, muted, and vanished tabs listing the connected server's
    players, offline ones included, connected ones first with their online
    state, and a search field while the tab has anyone; a row's button
    removes the row at once with the menu's effect. A click whispers an online
    player, and a right click opens the player's menu, an offline player's
    with whisper disabled. Escape closes the window and leaves the chat open;
    closing the chat closes it too. Another server lists only its own
    players.
22. Adding a friend from its menu, anywhere, adds it to the friends tab with
    no notice, and the menu then offers to remove it; the other player is
    told nothing. When that player next connects, a notice says so and the
    chat's lines do not; friends already online when this game connects are
    not announced. An offline friend shows its last name. A friend stays one
    after either game restarts, and muting or vanishing a friend keeps the
    friend.
23. Activate on another player's character, or the horse it rides, shows a
    cursor and its [player menu](#player-menu) at the crosshair, and no
    dialogue starts. Whisper opens the chat whispering that player. Escape,
    Activate again, or a click outside closes the menu without opening the
    journal or activating anything, and the character moves again at once.
24. A click on an accent swatch changes every accented element at once, the
    frames too with `BorderColor = Accent`, and saves `AccentColor`, which
    the next game loads. The custom picker changes only the custom swatch
    until release, and a typed hex color applies on Enter. A dark accent gets
    light text on the accent fill. The status panel does not change.
25. A player's menu looks the same wherever it opens: its name in the accent
    with its level, then the place, also for a player far away, whose details
    fill in without the menu changing height. A place from a plugin the local
    game lacks shows as unknown.
26. A new install connects to the official server, whose address appears
    nowhere in the settings window or `PlayerHub.ini`. Switching
    `CustomServer` on while connected keeps the connection and shows the
    address field and `ServerTls`. Disconnected with the field empty,
    connecting is disabled with a hover reason. A typed host applies on
    Enter, and connecting joins it on the default port, or on the port typed
    after a colon, over TLS with `ServerTls` on. An address with a scheme, a
    path, or a port outside `1..65535` is invalid. Switching `CustomServer`
    off keeps a live custom connection; the next connect joins the official
    server.
27. Activating a nearby player and picking trade shows both players the
    prompt, which the trade keys answer, without moving the character or
    opening a game menu. Accepting opens the trade window on both screens,
    also over an open chat. A click on a stack asks how many and only the
    popup's button or Enter offers them; they show in the own grid with their
    count. A click on an offered row or cell changes or removes its line; a
    cell that offers one item removes it at once, and right clicks do nothing.
    The other window shows changes within a moment, and an accepted side is
    marked. A modded item the other game lacks never appears in either offer,
    its giver gets a notice naming the plugin, and its row is disabled.
    Escape, Cancel, and a message box or dialogue a script opens each close
    both windows. The chat stands left of the panel whispering the other
    player; `OpenChat` or a click on it, also with `OpenChat` unbound, lets
    the player type there; a line sent keeps it focused; Escape returns to the
    trade, and a click on the panel leaves the chat with its text kept and
    does what it would without it. Enter in the search field or a count popup
    does not focus the chat. At 16:9 and 21:9, at any `Scale`, the layout is
    the same, centered, with nothing cut off.
28. Both grids fill with textured thumbnails of the offered items within a
    second or two and keep them, with a steady frame rate. With a full offer
    every cell is filled, the grid does not scroll, the full mark shows and
    goes when a line is removed, rows not yet offered are disabled without a
    notice but still preview, and an offered row's count can still change.
    With `ItemPreview` off the cells show names and the preview column only
    the card. The inventory looks normal afterwards. A thumbnail and the
    model look equally bright in any light, with no face left black, and the
    world's lighting does not change while they capture.
29. Notices stack on the right edge: a completed trade, the other player's
    decline, a friend connecting. Each fades after a time that grows with its
    length, the same notice twice shows twice, and a fifth pushes out the
    oldest. They show over menus and the trade window and never add a chat
    line.
30. The open trade takes the screen at any resolution and aspect: the game
    behind it is blurred and darker and keeps moving, the panel takes the
    same share of the screen's height, and the game looks normal after the
    window closes, with a steady frame rate meanwhile. Hovering a row or cell
    shows its model at once, textured and anti-aliased over the blur as the
    inventory menu shows it, and its card laid out as the inventory menu's;
    moving off the panel keeps it. The model spins evenly without flicker;
    dragging stops the spin and turns it like the inventory menu's model, and
    on release it spins on from there, keeping its angle after other items
    were hovered and thumbnails captured.
31. `[Controls]` lists every key setting with its key. A click on a key's
    button, then a key, binds and saves it, and the action follows at once.
    While a button listens, another action's key is named and not bound, and
    Escape or a click cancels, neither closing the window nor reaching the
    game. Rebinding `OpenSettings` moves opening and closing to the new key. A
    display name typed with a letter bound to `OpenSettings` keeps the letter
    and the window open. Backspace unbinds and saves an action, whose hint
    the trade prompt then leaves out, and restores the default for
    `OpenSettings` and `OpenChat`. Defaults restores every key.
32. With `OpenChat` bound to a letter, that letter opens the chat with an
    empty field and Enter reaches the game. `ManagePlayers` opens the player
    manager alone with the cursor, the key again changes nothing, and Escape
    or its close button returns input to the game; a click on an online
    friend there opens the chat whispering that friend. Unbound, its key
    reaches the game.
33. Switching the Windows input language while the chat's field is open
    changes the layout of the next letters without closing the chat, and the
    log shows the new layout with the first character typed in it.
34. Another player in view within `NameplateDistance` has their display
    name above their head, following it in every stance, mounted, and in
    first and third person; a friend's name is in the accent, changing at once
    when added or removed. Behind a wall or hill the name fades out within
    about half a second, and fades in on returning to view; near the range's
    end it fades out. Turning the camera to a player shows the name almost at
    once. No name shows in menus, dialogue, with the HUD hidden, behind the
    trade window, or with `Nameplates` off. Many players in view do not lower
    the frame rate noticeably.
35. Releasing the `Scale` slider resizes the text and every overlay window,
    notices, and nameplates from the next frame; the open settings window
    grows around its center and stays on the display, and the cursor speed
    does not change. An invalid value in the file warns and loads the
    default.
36. In a game with no display name, the [display name hint](#display-name-hint)
    names the `OpenSettings` key, following a rebind, and is hidden in menus,
    dialogue, with the HUD hidden, and while an overlay window is open. The
    key opens `[Connection]` with the keyboard in the field, and applying a
    name removes the hint; clearing it brings the hint back.
37. The chat position sliders place the open chat and the recent lines in the
    same column at any resolution; a chat dragged wider grows right up to the
    screen's edge, and one too large for the room is drawn smaller while
    `PlayerHub.ini` keeps its size. At `100` and `100` the chat is at its
    minimum size, wholly on screen, with the player list on its left. The
    positions survive a restart.
38. The settings window first opens centered. Every corner and edge resizes
    it, keeping the opposite one in place. Closed and opened again in the
    same game session it keeps its place and size; after a restart it is
    centered again.
39. Dragging the open chat by a part that takes no click moves it with the
    move cursor, keeping its size and stopping at each screen edge. A press on
    a filter, chip, the field, the scrollbar, or a name does what it did and
    moves nothing, as does a click without a drag. On release the chat stays
    where it was dropped, the position sliders follow, and the place survives
    a restart. Dragged into the upper right corner, the player list opens on
    the chat's left.
40. With `sLanguage=GERMAN`, the overlay's text is German, the log has
    `Localization: de.po with <n> of <n> texts translated (0 fuzzy, 0
    rejected) for GERMAN` and `Localization: the game's language GERMAN is
    de`, and `PlayerHub.ini` keeps its English comments. With
    `sLanguage=JAPANESE`, Han characters have their Japanese shapes and a long
    Japanese effect description wraps inside the item card. A language without
    a table shows English. Picking another language in `[UI] Language`
    switches the open windows in the next frame, saves the code, and survives
    a restart, also changing the Han shapes; Defaults returns to the game's
    language. A community `.po` made from the deployed template appears in the
    combo by its `LanguageName` after a restart, with untranslated texts in
    English; removing it logs that no file exists and shows the game's
    language.
41. Dragging `BackgroundOpacity` changes the background of the settings
    window and its lists and tooltips together from the next frame; on
    release it saves, and every surface in
    [Background opacity](#background-opacity) follows, while the status panel
    and the recent lines do not. An invalid value in the file warns and loads
    the default.
42. Below full opacity, every translucent surface shows the scene blurred
    behind it, the blur ending at each frame's silhouette without a seam and
    fading with a notice, and moving with the camera; over the trade window,
    popups match the darkened backdrop. At full opacity nothing is blurred.
    The status panel and recent lines are never blurred. The frame rate with
    the chat open does not drop noticeably.
43. While the player is heard, the microphone shows at its configured
    position, wholly on screen at any position and resolution. With
    `Indicator` off it never shows and its position sliders are hidden;
    pointing at either slider previews the microphone without speaking and a
    drag moves it. Defaults restores and enables it, and the values survive a
    restart. Without `PlayerHub.ttf` it never shows.
44. In an open trade, the microphone shows over the blurred backdrop while
    the player is heard, and the trade's windows draw over it where they
    overlap.

## Known limitations

- Another plugin's [held input](#input-focus) is learned from the last list of
  keyboard or mouse input it dispatched. A focus key pressed in the dispatch
  that opens that plugin's menu, or right after it closes with no keyboard
  or mouse input in between, gets the previous state. A plugin that empties
  a list for any other reason, such as dropping its only key, blocks focus
  keys until the next list it passes on. The [trade window](#trade-window)
  still takes focus while another plugin holds input, and the other menu
  then gets no keyboard or mouse input until the trade window closes. A
  plugin that blocks input some other way, or whose detour runs before
  this one and empties the list, is not detected; in the second case focus
  keys never reach the overlay anyway.
- Under Community Shaders' frame generation the [backdrop](#backdrop) and
  [translucent surfaces](#background-blur) blur the scene without the HUD or
  menus, which draw on the UI layer. Under its HDR output without frame
  generation they are not blurred.
- A [blurred](#background-blur) popup or tooltip over an overlay window shows
  the game behind that window, blurred, rather than the window. The blur's
  strength and darkening are fixed.
- After a [language switch](#localization), text composed before it, such
  as chat lines, notices, and trade rejection reasons, stays in the earlier
  language.
- The shipped translations are machine drafts. Dear ImGui draws neither
  right-to-left layout nor glyph shaping, so a translation into Arabic,
  Hebrew, or an Indic script loads but does not display correctly, and a
  script without a system font shows no glyphs. Numbers use
  `std::format`'s defaults, not the language's decimal separator.
  Translation files are read once at startup: a new or edited file needs a
  restart.
- The first time a player's menu opens in a run, it shows the level and
  place a game pass later for a player with a character in range, and a
  server round trip later for anyone else; later opens show the known ones
  at once. Opening more than five menus within a few seconds can exceed the
  server's [inspection limit](network.md#inspection), and those menus show
  the known details, or the name alone for a player not seen before.
- Any accent is accepted, so a very dark one or one close to the surfaces
  makes selection and hover marks hard to see. The accent follows a picker
  drag only on release, since each apply saves the file.
- `[Connection]` has no Defaults button ([Settings window](#settings-window)).
- A window over the right edge, such as the trade window, hides the notices
  under it; they keep their time meanwhile.
- Nameplates are tested from the camera to the actor, so a name stays shown
  while only part of the character is in sight, and keeps its result for up
  to 0.2 s after the view changes. Plates of players standing close
  together overlap; nearer ones are drawn over farther ones. A plate shows
  while another plugin's menu that neither pauses nor hides the HUD is open.
- Lists, tooltips, notices, notes, and recent chat lines have no menu frame,
  since its corners are taller than a line or two. Nothing marks where a
  window resizes or where the chat can be grabbed to move it but the cursor.
- `BorderThickness` sizes the whole frame; thinner lines with knots of the
  same size would need another shape. The frame is exported ahead of time
  from the vanilla `messagebox.swf`, so a mod that replaces the game's menu
  art does not change it.
- Keys are keyboard keys only: mouse buttons, the gamepad, and keys with a
  modifier cannot be bound. Labels are a US layout's, since scan codes are
  positions. A key a mod or the game also uses is bound without a warning;
  only the plugin's own keys are checked against each other.
- A focused window keeps its keys only from plugins that take them from the
  input dispatch call. One that reads the keyboard another way, such as
  `GetAsyncKeyState`, the window procedure, or DirectInput, still sees them.
  Another plugin's menu that neither pauses the game nor pushes an input
  context does not block focus keys.
- A script none of the system fonts cover shows placeholders; emoji and an
  input method editor's composition cannot be typed, since Skyrim takes only
  single `ToUnicode` characters. In a themed window a character Sovngarde
  lacks is drawn in the system font, so a word can mix the two faces.
  Sovngarde's `1` and `l` look alike, which shows in the server address.
- Editing shortcut letters (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY
  positions, not the keyboard layout. Chat lines and player names cannot be
  selected or copied.
- The field shows a mention as plain text while it is written. A mention is
  found against the players online when its line arrives, so naming a player
  who connects a moment later leaves it plain.
- The chat is resized and its filters toggled with the mouse only. A word
  wider than the chat is cut at the row's end, not at a syllable. A click
  made before the mouse has moved since the chat opened is dropped.
- The player list sorts by folded bytes, so `Aela 194` comes before
  `Aela 2`, cuts a long name without an ellipsis, and is picked from with
  the mouse only. Names in scripts other than Latin, Greek, and Cyrillic
  match only in the case they are written in.
- The send target, the filters, the whisper partners, and the sent lines last
  until the game exits.
- An offline player in the player manager shows the name the roster last gave
  it while this game ran, or the kept one. The window lists the last server
  that welcomed the client, or the configured address's before the first
  connection of a game, so an address changed while disconnected shows its
  list only once connected. Friends, mutes, and vanishes are kept per server
  URL, so the same server reached under another address starts with none.
- A gamepad's Activate opens the player menu, but the menu takes the mouse
  and keyboard only, while gamepad input keeps reaching the game.
- The picker covers armor, weapons, ammunition, and races, not torches,
  spells, or actors; `ItemKind` is where more kinds would go.
- A trade thumbnail or model shows the base form's model, without a player
  enchantment's glow or the item's own name. An item whose textures were
  missing at the first capture shows untextured until its refresh; one still
  missing them then stays so until it is shown again. A full grid fills over
  a moment, and while the hovered item's model loads thumbnails wait, so
  sweeping the cursor across rows delays them. The atlas holds 64; more
  distinct items in view than that show names.
- A display narrower than about 1.07 times its height leaves no room for the
  preview column, and the trade then shows no item numbers. The model cannot
  be zoomed, and a fast drag turns in larger steps than the inventory menu's.
  The spin re-renders the model up to every `kSpinCaptureIntervalSeconds`
  and pauses while thumbnails are captured.
