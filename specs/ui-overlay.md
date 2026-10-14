# UI Overlay

## Status

- **Overlay, input, status panel:** implemented and built on Windows; the
  settings UI runs in game. The renderer and input hook sites and the input
  context order were checked against the 1.7.104 executable and address library.
  SE 1.5.97 uses the same IDs and offsets as other public ImGui overlays but was
  not checked here. Skyrim VR is not supported: the hooks are not installed and
  a warning is logged. The status panel showing only the disconnected state
  without a display name is not built on Windows or run in game; on Linux,
  `StatusPanel.cpp` passed a syntax check against Dear ImGui 1.92.9.
- **Number sliders and section defaults** (2026-10-13): not built on Windows or
  run in game, and not run in the headless harness. On Linux,
  `SettingsWindow.cpp`, `SettingsSchema.cpp`, and `PluginConfig.cpp` passed
  the g++ syntax check. That Dear ImGui draws a format without a conversion
  as it is follows its demo's enum slider and was not run here.
- **Chat position** (`[UI] ChatPositionHorizontal` and
  `ChatPositionVertical`, 2026-10-13): as offsets from the former default place, with the size limited to the room
  from the chat's corner and the player list's side choice, it worked in game
  on Windows (the user's check). As positions from the screen's corner, without
  the margins, it is not yet built on Windows or run in game; on Linux,
  `ChatWindow.cpp`, `SettingsSchema.cpp`, `PluginConfig.cpp`, and
  `SettingsWindow.cpp` passed the g++ syntax check. Moving the chat by a drag
  (2026-10-13) is not built on Windows, run in game, or run in the headless
  harness; on Linux, `ChatPosition.cpp`, `ChatSize.cpp`, and `ChatWindow.cpp`
  passed the g++ syntax check, which a planted error confirmed reaches
  `ChatPosition.cpp`.
- **Settings sections** (`[Connection]`, `[Advanced]`, and the `[UI]`
  groups, 2026-10-13): not built on Windows or run in game, and not run in
  the headless harness. On Linux, `SettingsSchema.cpp`, `PluginConfig.cpp`,
  `SettingsWindow.cpp`, `ConnectionControls.cpp`, `DisplayNameHint.cpp`,
  `StatusPanel.cpp`, `ConnectionMonitor.cpp`, `ServerClient.cpp`, and
  `ActorLifecycle.cpp` passed the g++ syntax check, which a planted error
  confirmed reaches `SettingsWindow.cpp`. The checks below that name
  `[General]`, `[Network]`, or `[Performance]` ran before the merge.
- **Settings window placement** (kept across openings, 54 × 36 font sizes,
  2026-10-13): not built on Windows or run in game. On Linux, the real
  `MenuFrame.cpp` ran in a headless Dear ImGui 1.92.9 test set up as the
  settings window: it opened centered at 54 × 36 font sizes; moved, resized,
  closed for a few frames, and reopened, it had the same rect; reopened after
  the font size changed from 13 to 16 px while closed, it was scaled by that
  ratio around its kept center and held there; scaling back while open still
  followed. The committed `MenuFrame.cpp` failed the reopen and closed-scale
  checks in the same test.
- **Display name field filter and Enter to connect** (2026-10-14): not built
  on Windows or run in game, and not run with the real `ConnectionMonitor`.
  On Linux, `SettingsWindow.cpp` and `ConnectionControls.cpp` passed the g++
  syntax check, which a planted error confirmed reaches `SettingsWindow.cpp`.
  `FilterDisplayName`, copied from it, ran in a headless Dear ImGui 1.92.9
  test on the field's `InputTextWithHint` call: typed punctuation, spaces,
  and non-ASCII letters are dropped; typing stops at 15 characters; a paste
  is filtered and cut to 15, also into a full field; Enter and keypad Enter,
  with and without an edit, end editing in the frame the key is pressed, with
  the edit applied in that frame. The notice naming the minimum length
  passed the syntax check only.
- **Display name hint** (2026-10-13): not built on Windows or run in game.
  On Linux, `DisplayNameHint.cpp`, `GameView.cpp`, `NameplateLayer.cpp`,
  `MenuFrame.cpp`, and `TradePrompt.cpp` passed the g++ syntax check, which a
  planted error confirmed reaches the hint's file.
- **Chat filter and send target tooltips** (2026-10-14): not built on
  Windows, run in game, or run in the headless harness. On Linux,
  `ChatHistory.cpp` and `ChatInput.cpp` passed the g++ syntax check, which a
  planted error confirmed reaches `ChatHistory.cpp`.
- **Shared window setup and search field** (`MenuFrame::BeginWindow`,
  `Controls::SearchField`, `Controls::MutedTextWrapped`, 2026-10-07): Linux
  syntax check only; not rerun in the headless harness or in game.
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
- **Input language** (2026-10-12): works in game on Windows (1.7.104),
  reported by the user: switching between English and Russian while the
  chat's field has focus changes the letters typed at once. The call sites
  in `BSWin32KeyboardDevice::Poll` were read from the 1.7.104 executable.
- **Nameplates** (2026-10-12): not built on Windows or run in game. On
  Linux, `NameplateSync.cpp`, `NameplateLayer.cpp`, `SyncCoordinator.cpp`,
  `ChatRoster.cpp`, and `SettingsSchema.cpp` passed the g++ syntax check,
  which a planted error confirmed reaches the first three. The player's
  line-of-sight path and the projection were read from the 1.7.104
  executable ([Engine assumptions](#engine-assumptions)).
- **Font scale** (2026-10-12): not built on Windows or run in game. On
  Linux, `Overlay.cpp`, `MenuFrame.cpp`, `SettingsWindow.cpp`,
  `SettingsSchema.cpp`, and `PluginConfig.cpp` passed the g++ syntax check,
  a planted error confirming each is checked. `MenuFrame`'s
  `FollowFontSize` ran in a headless Dear ImGui 1.92.9 window set up as
  `BeginWindow` sets up the settings window: a window dragged and resized at
  100% grew to 130% around its center (900 × 600 to 1181 × 787 at a 21 px
  font) and held on later frames, and a window in the display's corner
  scaled from its size before `Begin` raised it to the new minimum and
  stayed on the display.
  The slider's step and the file's step check were not run.
- **SE 1.5.97 IDs** (2026-10-12): a 1.5.97 crash log showed opening a
  menu with an item preview crash in `Forms::GetItemDetails`, which called
  1.7.104 Address Library IDs that resolve to unrelated functions on SE; the
  item details formulas are now ported (see [Trade](trade.md#item-details)).
  The SE IDs of the preview's turn (50902) and ambient override (98989,
  527705), the input language patch's poll (67472, `ToUnicode` at +0x20D),
  and the nameplates' line of sight and projection were matched against
  the 1.5.97 executable's disassembly, function by function with their
  1.7.104 counterparts. Not built on Windows or run on 1.5.97:
  `ItemDetails.cpp`, `ItemPreview.cpp`, and `InputLanguage.cpp` passed the
  g++ syntax check, which a planted error confirmed reaches each.
- **Notices and notes:** not built on Windows or run in game. On Linux, the
  changed `sync/` and `ui/` files passed a g++ 16 syntax check against
  CommonLibSSE-NG, and `NoticeStack.cpp`, `Notices.cpp`, `ChatComposer.cpp`,
  `ChatInput.cpp`, and `Theme.cpp` ran in a headless Dear
  ImGui 1.92.9 context with stubbed roster, filters, and sending, without
  the themed fonts or the frame (47 checks): a fifth notice dropping the
  oldest, a friend's arrival, a repeat shown as a toast of its own, the
  strip in the accent, 5 s for a short
  notice and 8.9 s for the 105-character save failure, half alpha halfway
  through the fade, nothing drawn once expired or outside a game, the
  stack's right edge and top where this spec puts them; a failed send
  keeping its text, an edit, a recalled line, and Tab clearing the notes; the panel ending one item spacing above
  the field and as tall as its wrapped rows, and gone after `OnOpened`.
  The rasterized stack and panels were looked at. `ChatWindow`'s routing of
  rejections, the connected notice, and the texts without final periods ran
  only through the syntax check.
- **Chat:** not built on Windows or run in game. Server unit tests cover
  routing, the roster, and whisper addressing ([Network](network.md#status)).
  On Linux, with g++ 16 and a logging stub:
  - Removing the chat commands, which folded `ChatDraft` into `Composer` and
    left notes failure-only, passed only the g++ syntax check, which a
    planted error confirmed reaches `ChatComposer.cpp` and `ChatInput.cpp`;
    the harness runs below predate it.
  - `ChatRoster.cpp`, `ChatComposer.cpp`, and `ChatSync.cpp` ran against a stubbed transport: the `@` prefix; names compared without
    case in ASCII, Latin Extended-A, and Cyrillic; the order of the name
    list; the longest-name rule; the prefix switching the target as it is
    typed; a name binding at its space only when no longer name continues
    it; Tab order with and without partners; a partner under another
    slot found by name and one who left reported offline; sent lines recalled
    with their targets; the messages for a player who left, text that cannot
    be sent, no connection, and the server's `unknown recipient`, checked
    when they were chat lines (see **Notices and notes** for what ran since).
  - `ChatWindow.cpp`, `ChatHistory.cpp`, and `ChatInput.cpp` ran with those,
    `Theme.cpp`, `MenuFrame.cpp`, and `Fonts.cpp` in a headless Dear ImGui
    1.92.9 context against a folder laid out as the deploy step leaves it,
    with simulated keys and mouse and the draw data rasterized to images, at
    1080p, at 4K, with `BorderColor = Accent`, and without `PlayerHub/`:
    typing, Tab, and Shift+Tab; the name list by
    keys and by a click; Escape closing the list, then the chat, with the
    text kept and discarded; Up and Down; each filter
    turning its channel off and on by itself, in the open chat and among the
    closed chat's lines, with the send target unchanged; the unread dots; a
    target and the player's own line turning a channel back on; a click on a sender's name; the chip's menu; a
    line's menu; a click outside; a whisper to a player who
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
    without `PlayerHub/`: a line longer than its row starting beside its
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
    its `[UI]` page has no row for either key.
  - `[UI] CloseChatOnSend` ran in a smaller harness with the chat's sources
    and simulated keys: on, Enter sending and closing and Enter in an empty
    field closing without sending; off, Enter sending with the chat open and
    the field keeping the keyboard for the next line, Enter in an empty or
    blank field leaving it open without sending, Escape
    closing it, Escape after sending a draft kept from the last opening not
    bringing it back, which fails without that rule, and Up recalling the
    line sent; turned on while open, the next Enter closing it.
    `SettingsWindow.cpp` ran with the real schema: the `[UI]` page has a
    switch for the key, and clicking it applies and saves each value.
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
  - The [player list](#player-list) ran in that smaller harness with
    `ChatRoster.cpp` over a stubbed transport, at 1080p and 4K, in Dear
    ImGui's default font and in Sovngarde, the draw data rasterized to
    images: the button on the filters' row, as high as the filters and
    ending at the field's column with 5 and with 500 players; the list beside
    the chat and as high, its search field on the filters' row and its rows
    a line apart; players nearby first and in the Nearby color, moving back
    among the others when no longer nearby; the local player left out, and
    the empty-list row when alone; a click on a name
    and the whisper item in a row's menu whispering that player, with typing
    reaching the chat's field after either; Escape closing a
    row's menu only; the search matching without case, the row for no
    match, Backspace, Tab kept in the field, and nothing typed there
    reaching the chat; Enter there giving the keyboard back to the chat's
    field; the button closing the list and clearing its search; Escape with
    a row's menu open closing only the menu; after Enter closes the chat,
    the list showing again when it opens; with 500 players only the rows in view
    drawn, the wheel over the list scrolling it and not the lines, and
    players nearby copied every half second; a chat dragged to the screen's
    right edge leaving the list room; the disconnected row without a
    roster. The resize checks of the bullet above passed again. With 300
    lines and 500 players a frame took about 97 µs with the list closed and
    100 µs with it open, against 127 µs for the chat before it and the
    [menu frame](#menu-frame)'s kept triangles; a join or leave in every
    frame, 150 µs.
  - An earlier drag ran in game, runtime not recorded: it moved in whole
    font sizes and lines with the window as high as its content, and the
    player saw it resize at a lower frame rate than the settings window,
    with the field and the chip flickering. The drag described under
    [Size](#size) is not built on Windows or run in game.

  Not run anywhere: the wheel over the lines with the cursor shown, the
  Windows clipboard, the resize cursors as the overlay draws them in game,
  and the player list with a real roster and players nearby.
- **Settings window, theme, connection controls:** an earlier layout, with tabs
  and a title bar, ran in game. The sidebar layout, the display name's line
  with the connection button, the switches, and the theme are not built on
  Windows or run in game. On Linux, `SettingsWindow.cpp`,
  `ConnectionControls.cpp`, `Controls.cpp`, `ItemPicker.cpp`, and `Theme.cpp`
  ran in a headless Dear ImGui 1.92.9 context against stubbed `Config::Apply`,
  `Network`, and `ItemPreview` functions and a fake catalog, with simulated
  mouse and keyboard input and the draw data rasterized to images over a
  stand-in scene. With `[General]` as a row and a button below it:
  - every section at 1080p, `[General]` at 720p and `[DefaultOutfit]` at 4K, the open
    sex and item lists, a label's tooltip, an invalid field with its
    footer, and a picked item with its reset button;
  - typing applies nothing; Enter and a click on another section each apply
    the text once; Escape puts the previous text back and leaves the window
    open; invalid text is not applied; a switch and a step button apply at
    once; an outfit pick applies only on Apply, and stays pending through an
    edit in another section;
  - the close button and Escape close the window; a drag on the sidebar or
    beside the title moves it and one on the lower right corner resizes it.

  `[General]` ran the same way at 1080p only, with the real schema and a
  stand-in item picker. Its layout ran against stubbed `Network` functions,
  before the field was locked and the connection button became the only way
  to connect:
  - the field and the button share a line in every state; the field has the
    keyboard when the window opens without a display name and not with one;
  - invalid or emptied text disables connecting, and a click does not connect
    with it; Escape puts the name back; disconnecting and cancelling stop
    wanting a connection, and a press while disconnecting does nothing.

  `[General]` as it is now ran with the real `ConnectionMonitor` over a
  stubbed transport and game state:
  - typing applies nothing and enables connecting; a click elsewhere applies
    the name and does not connect, also in a game started with `AutoConnect`
    and no display name; one click on the button applies a typed name and
    connects, once; Enter did not connect when this ran;
  - while connecting and while connected the field is disabled, a click,
    typing, and Enter change nothing, and hovering it shows its tooltip;
    disconnecting unlocks it, and a name applied then does not connect;
  - a refusal for the display name, for the protocol version, and of a
    replaced session each leave the button connecting, an editable field, and
    the refusal under it; applying another name keeps the refusal and does not
    connect; connecting clears it.

  ImGui's style-stack checks passed. The mouse path of `InputCapture` was not
  run there. Closing with `OpenSettings` is not built on Windows or run in game;
  on Linux, `InputCapture.cpp` and `WindowManager.cpp` ran against stubbed
  engine input and ImGui IO functions: the key opens and closes the window
  without the game seeing either press, an editing key bound as the settings
  key stays with an active text field, and chat keeps Enter.
- **Themed faces (Sovngarde):** not built on Windows or run in game. On Linux,
  `Fonts.cpp` ran with POSIX stand-ins for its Win32 file calls, with
  `SettingsWindow.cpp`, `ConnectionControls.cpp`, `Controls.cpp`, and
  `Theme.cpp`, in a headless Dear ImGui 1.92.9 context against a folder laid
  out as the deploy step leaves it and stand-in system fonts, the draw data
  rasterized to images:
  - every section at 1080p and `[DefaultOutfit]` at 4K, and a
    label's tooltip are in Sovngarde with bold headings; a display name with
    `Ґ`, `ž`, and Han characters shows them from the fallbacks;
  - without `PlayerHub/fonts/` the log warns for each file and the window is
    in the system font; without system fonts both faces still load;
  - after a frame the font stack is empty and the current font is the
    overlay's.

  The `xmake.lua` deploy step and a file that is not a font were not run. The
  chat in Sovngarde is under Chat above.
- **Logo:** not built on Windows or run in game. `assets/logo/generate.py`
  builds the same `PlayerHub Logo.ttf` on every run. On Linux, a headless Dear
  ImGui 1.92.9 context loaded it beside Sovngarde and drew `DrawBrand`'s calls
  in the sidebar's layout under the real [menu frame](#menu-frame), the draw
  data rasterized to images: at 720p, 1080p, 1440p, and 4K the logo starts at
  the navigation labels' left edge and ends 2 to 4 px inside the right inset
  (137 × 30 px at 1080p, 278 × 61 px at 4K), anti-aliased, with the scratches
  showing through the bar and the navigation below it; it is clear of the
  knots at `BorderThickness` 70 and 100. `Fonts.cpp` and
  `SettingsWindow.cpp` themselves did not run there.
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
    `Accent` for `[UI] BorderColor` applies it and both frames are orange
    from the next frame;
  - the `[UI] BorderThickness` slider shows 70% and has no step buttons; a drag
    past its right end gives 100 and past its left end 50, one to its middle
    75, and the frame and the content inside it keep the previous value while
    the button is held and follow the new one once it is released, at 1080p
    and 4K; Ctrl and a click make it a field, where typing applies nothing
    until Enter, 85 sets 85, 150 gives 100, and 7 gives 50;
  - a drag on the lower right corner resizes the settings window and the frame
    follows, and the cursor over that corner is the resize one; recent chat
    lines have no frame and stand in the open chat's column;
  - without `PlayerHub/frame/` the log warns and both windows have their
    style's rounded rectangle with a line in the frame color; a file cut
    short, of another version, with a `nan`, with a point
    count out of range, or with no polygons each warn with the reason and fall
    back the same way; a file with CRLF line ends loads.

  Keeping a frame's triangles ran in the chat's harness at 1080p and 4K:
  images with and without it are the same but for one pixel's rounding,
  through opening, hovering, menus, searching, and a drag; the open chat
  took about 97 µs a frame instead of 127 µs, and drags as long as before.
  The `xmake.lua` deploy step was not run. `[UI] BorderColor` in the file is
  under [Configuration](configuration.md#status).
- **Accent color:** not built on Windows or run in game. On Linux,
  `SettingsWindow.cpp`, `ConnectionControls.cpp`, `Controls.cpp`, `Theme.cpp`,
  and `MenuFrame.cpp` ran with the real `PluginConfig.cpp` and
  `SettingsSchema.cpp` (saving to a scratch folder) in a headless Dear ImGui
  1.92.9 context with stubbed `Network`, fonts, and item picker, simulated
  mouse and keyboard input, and the draw data rasterized to images at 1080p:
  - the default accent, its text color, and the `Accent` frame are the
    `FF9000`, `kOnState`, and `DB7D03` the theme had before;
  - `[UI] AccentColor` shows eight swatches and the custom swatch evenly across the
    control, ending at its right edge, no wider than the row is high, and
    still apart at the window's minimum width;
  - a click on the blue swatch applies and saves `4F8EF7` in one apply, and
    the frame, sidebar mark, switches, and slider turn blue from the next
    frame;
  - the custom swatch opens the picker below it, right-aligned; a drag across
    its square applies nothing until release and then once; typing in its hex
    field applies nothing until Enter, and `203040` gives light text on the
    accent; Escape first leaves the hex field with the color kept, then
    closes the picker, then the window.

  ImGui's style-stack checks passed. The chat files that use the accent
  (`ChatPlayers.cpp`, `ChatPlayerManager.cpp`, `ChatWindow.cpp`) passed a
  syntax check only. `[UI] AccentColor` in the file is under
  [Configuration](configuration.md#status).
- **Key bindings:** not built on Windows or run in game. On Linux,
  `SettingsWindow.cpp`, `Controls.cpp`, `Theme.cpp`, and the real
  `SettingsSchema.cpp` ran in a headless Dear ImGui 1.92.9 context with a
  stubbed configuration, fonts, menu frame, connection controls, and item
  picker, simulated mouse input, and key presses handed to
  `Window::OnCapturedKey` as `InputCapture` would, checking the drawn text
  and items, not images: the `[Controls]` page lists `OpenSettings`,
  `AcceptTrade`, and `DeclineTrade` with `F10`, `Y`, and `N`, and `[UI]`
  lists no key; a click makes the button listen with no line under it, and
  `H` binds and applies once; `N` for another action shows the conflict line
  naming `DeclineTrade` and keeps listening, Escape then cancels without
  closing the window, Left Shift shows the reserved-key line, and the bound
  key ends listening without an apply; a click on the listening button, on
  another key's button, with the right button on the empty page, and on
  another section each end it, the second starting the other key's; of two
  keys pressed before a frame, the first binds; `Numpad7` for
  `OpenSettings` shows the keypad label and becomes the window's focus and
  release key; losing focus ends listening; the rows below keep their place
  when a button starts listening and move down for a rejection's line;
  Backspace unbinds `AcceptTrade`, which then shows as unbound, ends
  listening without an apply on an unbound action, and leaves two actions
  unbound without a conflict; Backspace on `OpenSettings` restores `F10`,
  and shows the conflict line while `AcceptTrade` has it; Escape without
  listening still closes the window. `InputCapture.cpp` and
  `WindowManager.cpp`, which route the key presses, passed a g++ syntax
  check only. The section's sidebar position (2026-10-11) passed the g++
  syntax check only; the harness ran when it came last. With `OpenChat` and
  `ManagePlayers` (2026-10-11) the harness also showed them between
  `OpenSettings` and `AcceptTrade` with `Enter` and `F11`, then the
  `ManagePlayers` default (now `O`, not rerun); `T` bound `OpenChat` and
  Backspace restored `Enter`; Enter for `ManagePlayers` showed the conflict
  line naming `OpenChat`, keypad Enter bound it as the keypad key, and
  Backspace unbound it. `ChatWindow.cpp`, which shows the manager alone, and
  the dropped character of a typing focus key in `InputCapture.cpp` passed
  the g++ syntax check only. The file side is under
  [Configuration](configuration.md#status).
- **Custom server switch:** not built on Windows or run in game. On Linux,
  `SettingsWindow.cpp`, `ConnectionControls.cpp`, `Controls.cpp`,
  `Theme.cpp`, and `MenuFrame.cpp` ran with the real `PluginConfig.cpp`,
  `SettingsSchema.cpp`, and `ConnectionMonitor.cpp` in a headless Dear ImGui
  1.92.9 context with a stubbed transport, fonts, and item picker and
  simulated mouse and keyboard input, checking the drawn text and items, not
  images. The current `CustomServer` switch ran in the settings-only harness
  (real `SettingsSchema.cpp`, stubbed configuration store and connection
  controls): `[Network]` shows the `CustomServer` and `AutoConnect` switches
  and no endpoint row or official URL; switching it on applies it and shows
  an unlabeled field with a URL placeholder between the two switches, from
  the labels' left edge to the switches' right edge; a typed URL applies on
  Enter and becomes the endpoint; switching off hides the field and goes
  back to the official server with the URL kept. The rest ran with the
  earlier, inverted switch, whose `ConnectionControls.cpp` behavior is
  unchanged, while disconnected: `ftp://x` turned on the footer line that
  invalid text then had, and hiding the row hid it, which stayed away when
  the row was shown again; the red line under the field that replaced it
  has not run. With the custom server and an empty field, `[General]`'s
  connection button is disabled with the reason then shown under it; a typed
  URL saves both lines, the button is enabled and the reason gone. The reason
  as the button's tooltip, the divider under the connection bar, and the
  display name's comment as the field's tooltip have not been compiled or
  run.
- **Mute and vanish:** not built on Windows or run in game; the server side is
  under [Network](network.md#status). On Linux, in the chat's headless Dear
  ImGui 1.92.9 harness with a stubbed transport and a deploy folder,
  `PlayerFilterStore.cpp`, `PlayerFilters.cpp`, `ChatSync.cpp`,
  `ChatRoster.cpp`, and every `ui/chat/` file ran: a kept mute loads for its
  server only and malformed lines are skipped; vanishing publishes once and
  writes `v`, muting too writes `mv`, undoing both removes the line, another
  server's list follows a new connection, and the 1025th vanish is refused; a
  mute hides the player's earlier lines and new ones on every channel, in the
  open and the closed chat, without a notice, and its whisper does not become
  a partner; unmuting shows them all again. In the player list a muted player's
  menu offers unmute and vanish, another's mute and vanish, the local
  player has no menu, and the marks show (rasterized). The manager's button
  sits at the list's bottom and opens a window in the middle of the screen
  over the chat, on the friends tab; the muted tab lists this server's muted
  players with an offline one marked as connected (then as text, since
  replaced by the [Friends](#status) checks' dot); unmute and unvanish
  remove their row at once; the search filters the tab shown and stays
  across tabs;
  Escape closes the window and leaves the chat open, a second one
  closes the chat; Enter closing the chat closes the window; its close button
  closes only it; dragged and reopened, it is in the middle again and on
  the friends tab (rasterized). With 500 players and 300 lines the open chat took the
  same time as before, about 107 µs a frame.
- **Friends:** not built on Windows or run in game; `ChatSync.cpp` and
  `ChatWindow.cpp` were only compiled. On Linux, in a headless Dear ImGui
  1.92.9 harness with a stubbed roster and store folder, the real
  `PlayerFilterStore.cpp`, `PlayerFilters.cpp`, `ChatRoster.cpp`,
  `ChatPlayerMenu.cpp`, and `ChatPlayerManager.cpp` ran: `f` lines load for
  their server only, `fx` and flagless lines are skipped, adding writes `f`
  and with a mute `fm`, and removing both drops the line; a connection's
  first roster announces nobody, a later join of a friend is announced and
  of anyone else not, a reconnect under a new slot announces nobody, also
  when the empty roster between was not seen; a renamed friend's new name is
  saved when it appears. The friends tab lists friends by name with a dot
  whose tooltip tells online from offline, and a remove button; a name under
  the cursor shows a hand, a click on an online one whispers it and on an
  offline one does nothing; a right click opens the menu with whisper and
  remove friend before mute and vanish, also on an offline friend (no race
  and level, whisper disabled) and on a muted tab's row; a close request
  shuts the menu first; the remove button and menu item drop the row at
  once, and an emptied tab shows its empty-tab line without a search field.
  Nothing was rasterized.
- **Player menu in the world:** not built on Windows or run in game.
  `RemoteActorActivation.cpp` and the `SyncCoordinator` step need
  CommonLibSSE-NG and were not compiled; the event they rely on was read from
  the 1.7.104 executable ([Remote actor](remote-actor.md#engine-assumptions)).
  On Linux, in the chat's headless Dear ImGui 1.92.9 harness with the real
  `WindowManager.cpp`, `PlayerActivation.cpp`, `ChatRoster.cpp`,
  `PlayerFilters.cpp`, and every `ui/chat/` file: an activated player's menu
  takes focus with the mouse and opens at the middle of the screen with
  whisper, mute, and vanish, the chat closed (rasterized); a player
  the roster does not list opens nothing; mute and unmute act and close
  it; Escape, E, a remapped Activate key (E then no
  longer), a left or right click outside, and focus lost to a menu close it;
  whisper opens the chat and its line goes to that player; an activation
  while the chat is open opens nothing and is not kept.
- **Player menu heading:** an earlier build that showed it in the world
  menu only, with the race read from the character, ran in game on Windows
  and the user saw the name, race, and level; the shared menu,
  the accent color, and `PlayerDetails` are not built on Windows or run in
  game. On Linux, a headless Dear ImGui 1.92.9 harness ran the real
  `ChatPlayerMenu.cpp`, `WorldPlayerMenu.cpp`, `PlayerActivation.cpp`, and
  `PlayerDetails.cpp` with stubbed peers, transport, and race names (text,
  heights, and vertex colors, not rasterized): the world menu of a player
  with an actor shows the name in the accent color, the race and level,
  whisper, and mute in that order without asking the server, at the
  same height before and after the details; the chat's menu of a player
  without an actor asks the server even with an old appearance kept, shows
  the reply at the same height, and drops a reply for another slot; an
  empty reply, no connection, and no reply after 2 s leave the name alone,
  a 5 s frame does not end the wait, and the chat's menu of a player with
  an actor is answered locally. `ChatHistory.cpp` and `ChatPlayers.cpp`
  passed a syntax check; `AppearanceSync.cpp` and `SyncCoordinator.cpp`
  were not compiled. The wire is in [Network](network.md#status).
- **Item picker and preview:** built on Windows; in game the picker lists names
  and the preview shows models. The attach pass for models that load in the
  background has not run in game yet. On Linux, `ItemCatalog.cpp`,
  `ItemPicker.cpp`, and the settings files ran headless with a fake catalog and
  a stand-in preview: per-key filters, multi-word and case-insensitive search,
  the preview following the hovered row, Enter picking the first match, Escape
  closing the list before the window, Apply saving `Plugin|0xID`, and
  `ItemPreview = false` hiding the preview. The reset button as an icon shown
  only off the default is not built on Windows or run in game. `ItemForms.cpp` and `ItemPreview.cpp`
  need CommonLibSSE-NG and were only checked in game. The
  [thumbnails](#3d-preview) ran in game (2026-10-07 log): every item was
  captured within a frame or two of loading, but some stills showed items
  without their textures. With the refresh capture (2026-10-08, in game)
  every thumbnail ended up textured; across 78 captures the texture check
  found nothing missing, so the refresh is what fixed them. The check on
  the attached model's form (2026-10-08) is not built on Windows or run in
  game: `ItemPreview.cpp` passed a g++ syntax check against the
  CommonLibSSE-NG headers with `__declspec` defined away, which a planted
  error confirmed reaches it.
- **Trade window and prompt:** not built on Windows or run in game. On
  Linux, `TradeWindow.cpp`, `TradePrompt.cpp`, `ChatPlayerMenu.cpp`,
  `WindowManager.cpp`, and `Controls.cpp` ran in a headless Dear ImGui
  1.92.9 context with a stubbed `TradeSync`, configuration, theme, frame,
  and preview (47 checks): nothing drawn without a trade; the prompt for an
  invitation, its name, keys, and countdown, taking no input; `Y` and `N`
  answering through `OnFocusKey` and giving input back, other keys left to
  the game, and only `N` taken while the player's own invitation waits; the
  window taking focus from another window, a click adding a
  stack, a blocked row adding nothing, Accept disabled with its blocker
  and while accepted, Escape cancelling, focus lost to a menu cancelling
  with `MENU`, a window kept from focus giving up with `FAILED` after 2 s but
  not at 1.8 s, a trade closed by the server releasing focus; the trade
  item disabled for a far player and while another invitation waits, accept
  and decline items for the inviting player, no entry for the local player. The window and prompt were rasterized at 1080p in Dear ImGui's
  default style, which found and fixed a heading hint that pushed the lists
  past the window. The real theme, fonts, and frame were not drawn.
- **Trade grids** (2026-10-07): not built on Windows or run in game. On
  Linux, `TradeWindow.cpp` with the real `Theme.cpp`, `MenuFrame.cpp`, and
  `Controls.cpp` ran headless in Dear ImGui 1.92.9 at 1080p (18 px font)
  against a stubbed `TradeSync` and `ItemPreview::DrawThumbnail` (67
  checks), and was rasterized: the list on the left, the peer's grid
  above the own one to its right, all inside the window, 83 px cells at the
  then default size of 56 × 40,
  thumbnails requested in offer order with the enchanted background, names
  when thumbnails are unavailable; Accept disabled with the inspect line
  until each peer cell was hovered, a cell inspected
  in the frame the cursor reaches it, a count change shading only that line, a `TradeSync` blocker disabling
  Accept with its reason as the button's tooltip and no footer line; a
  click opening the popup, its step button
  and remove button, Escape closing the popup
  and not the trade, the popup closing when its line leaves; Escape
  cancelling otherwise; the inspected lines reset by a new trade; the green
  tint. The
  prompt, focus, and menu checks above predate the grids and were not run
  again. `TradeSync.cpp` (`PeerLine::id`) passed only the g++ syntax check.
  A click on a single offered item removing it without the popup works in
  game (2026-10-09, user report). The count popup for a clicked stack in
  the item list and the removed right clicks (2026-10-09) passed only the
  g++ syntax check of `TradeWindow.cpp` and `TradeSync.cpp`. The full
  offer ignoring new rows (2026-10-09) passed only the g++ syntax check of
  `TradeWindow.cpp`. The gap between borderless cells (2026-10-10) passed only the g++
  syntax check of `TradeWindow.cpp`, a planted error confirming it is
  checked; it was not rasterized. A click removing a cell that offers one
  of a larger stack, and the popup's count removing the line at 0
  (2026-10-11), passed only the g++ syntax check of `TradeWindow.cpp`.
  The full mark on the own grid's heading and the unoffered rows
  disabled while it shows (2026-10-14) passed only the g++ syntax check of `TradeWindow.cpp`, a planted error confirming it is
  checked; it was not rasterized.
  The requesting prompt drawn as the inviting one, the accent name, and the
  fixed-width seconds (2026-10-13) passed only the g++ syntax check of
  `TradePrompt.cpp`, `MenuFrame.cpp`, and `TradeSync.cpp`, a planted error
  confirming it is checked. A headless test against ImGui 1.92.9 showed a
  reappearing auto-resized window rendering one frame at its old width, and
  `BeginPrompt`'s hidden appearing frame removing it. The two centered
  lines (2026-10-13) passed the g++ syntax check of `TradePrompt.cpp`, and
  a headless ImGui 1.92.9 test of the same line code centered the narrower
  line and kept the window's width as the seconds went from 30 to 9.
- **Trade screen, preview column, backdrop, and turning model**
  (2026-10-09): not built on Windows or run in game. On Linux,
  `TradeWindow.cpp` with the real `Theme.cpp`, `MenuFrame.cpp`, and
  `Controls.cpp` ran headless in Dear ImGui 1.92.9 (16 px font at 1080p)
  against a stubbed `TradeSync`, `ItemPreview`, and `Backdrop` (35 checks)
  and was rasterized at 1080p and 1280 × 1024: the fixed 56 × 44 panel to
  the right of a 28 font size column, the two centered, the model square
  above the card reaching the panel's bottom; a narrower column at 5:4,
  none on a square display, the same proportions at 4K; the backdrop drawn
  each frame; at once the hovered row's or
  cell's name, count (held or offered), stats, effects, and a
  blocked row's reason; the last item kept while the cursor is over the
  backdrop; a shown row found again after rows moved and the card cleared
  when its row or peer line left; a peer cell still inspected by hovering;
  a drag on the model square reaching it; clicks on the backdrop doing
  nothing; Escape cancelling. Cells came out at the 5 font size minimum.
  The inspect hint on the peer grid's heading line and the card left out
  until an item is shown (2026-10-10) passed only the g++ syntax check.
  `Gpu.cpp`, `Backdrop.cpp`, `ItemPreview.cpp`, `ItemPicker.cpp`,
  `MenuFrame.cpp`, and `Overlay.cpp` passed only the g++ syntax check, a
  planted error confirming it reaches `ItemPreview.cpp`. The HLSL compiled
  with DXC 1.9 for shader model 6 in HLSL 2016 mode, not with the FXC
  behind `D3DCompile`. The blur, the black and white capture, turning, and
  the picker's model on its panel were neither drawn nor run. The macro
  clash that broke the first Windows build (2026-10-10) was fixed, after
  which the user saw the trade screen in game and found it looking right
  but the model's resolution low; what else they tried is not known. The capture fitted to the
  drawn size (2026-10-10) passed only the g++ syntax check; capturing at
  1.2 times it did not look sharper in game and was reverted (the 08:35
  log showed captures already larger than the area, 705 px for 597). With
  the game's TAA off the model was sharp in game but its edges jagged (user
  report, 2026-10-10); the 2× capture drawn at half size that followed
  passed only the g++ syntax check. The user found the 2× capture much
  better in game (2026-10-10). Capturing [animated](#3d-preview)
  models on every update, for an enchanted item's glow flickering while
  turned, made it animate correctly in game, but the glow appeared only
  once the model was turned (user report and log, 2026-10-10); watching
  each enchanted load for 0.5 s, and enchanted thumbnails waiting for their
  glow, passed only the g++ syntax check. Starting each load from the
  game's transform and fitting thumbnails to 512 px, after the user saw
  stills missing (2026-10-10), passed only the g++ syntax check. The
  model's [spin](#3d-preview) (2026-10-11) passed only the g++ syntax check
  of `ItemPreview.cpp`, a planted error confirming it is checked. In the
  2026-10-14 log, stills of items added to the offer appeared up to 3 s
  late; the scheduler that serves a whole form from one load, puts pending
  stills before the model's refresh and glow watch, and bounds a still's
  glow wait (2026-10-14) passed only the g++ syntax check of
  `ItemPreview.cpp`, a planted error confirming it is checked.
- **Vanilla item card** (2026-10-10): not built on Windows or run in game.
  On Linux, `ItemCard.cpp` with the real `MenuFrame.cpp` and `Theme.cpp`
  ran headless in Dear ImGui 1.92.9 with Sovngarde (16 px at 1080p,
  Liberation Sans standing in for the system fonts) and was rasterized for
  seven cards of the column's 28 font size width, each as tall as its
  content up to 14 font sizes with no scrollbar: a charged
  weapon, a stack with a note, a Cyrillic name in capitals, and two that
  reach the limit and scroll (ingredients with four effects and a blocked
  reason, and an item with four effects and a three-line name), each also
  scrolled to its end with the bottom margin kept and only the child
  showing a scrollbar. The column moved to the panel's right (2026-10-10)
  passed only the g++ syntax check of `TradeWindow.cpp`, as did the panel
  20% taller (56 × 52.8 font sizes), whose cell size was estimated, not
  measured. `ItemCard.cpp` and `TradeWindow.cpp` passed the g++ syntax
  check against CommonLib, a planted error confirming it reaches both.

## Scope

The plugin's own UI layer: [nameplates](#nameplates) over other players'
heads, a status panel, the [display name hint](#display-name-hint),
[notices](#notices), chat, the
settings window, the
[trade window and prompt](#trade-window), and the reusable item picker with a
3D preview. `ui/` never changes the game; it reaches
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
  Fonts.h/.cpp                # system fonts with fallbacks, shipped themed faces and logo
  Theme.h/.cpp                # look of the interactive windows: accent, rounded borders
  MenuFrame.h/.cpp            # Skyrim's menu frame around a window, from its shipped shape
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
  chat/ChatWindow             # open and closed chat, focus by its keys, the manager alone, the send target's channel kept shown
  chat/ChatHistory            # lines, channel filters, unread marks, scrolling, the line menu
  chat/ChatInput              # target chip and its menu, text field, name list, notes
  chat/ChatSize               # the open chat's size, the drag that sets it, saving it
  chat/ChatPosition           # the open chat's corner, the drag that moves it, saving it
  chat/ChatPlayers            # the players online: the button on the filters' row, the list beside the chat
  chat/ChatStyle.h            # channel and failure colors, the chips' frame, the rule under the filters
  chat/ChatPlayerMenu         # a player's menu: heading, whisper, trade, friend, mute and vanish items or their undoing
  chat/ChatPlayerManager      # player manager: friends, muted, and vanished tabs, over the chat or alone
  chat/WorldPlayerMenu        # a player's menu at the crosshair, opened by activating its character
  trade/TradePrompt           # a trade invitation waiting for an answer, answered by its keys
  trade/TradeWindow           # the open trade: a fixed panel with both offers as grids, own items, Accept and Cancel, and the preview column
  trade/ItemCard              # the preview column's item card in the vanilla layout
  settings/SettingsWindow     # plugin settings: sidebar, section pages, footer
  settings/ConnectionControls # connection button, the line under it, one-line summary
  widgets/Controls            # switch, accent and red buttons, icon buttons, search field, muted text, striped box
  widgets/ItemPicker          # item dropdown
  preview/ItemPreview         # 3D item model, spinning and turned by dragging, and cached thumbnails
  render/Gpu                  # back buffer, runtime-compiled full-screen passes, image blend modes
  render/Backdrop             # the game's frame blurred and darkened behind a full-screen window
src/sync/chat/ChatSync        # send to a target and why it failed, the server's rejections, lines to display
src/sync/chat/ChatRoster      # connected players by name, completion, whisper partners, the list's copy
src/sync/chat/ChatComposer    # send target, the `@` prefix, name list, notes, sent lines
src/sync/notices/Notices      # the notices queue any feature posts to
src/sync/nameplates/NameplateSync # per peer: head node, line of sight, fade, label; the pass's plates
src/sync/filters/PlayerFilters # friends, muted and vanished players of the current server, publishing, friends coming online
src/sync/chat/PlayerActivation # the activated player, and a whisper picked in its menu
src/runtime/PluginTime        # plugin time per pass
assets/fonts/                 # Sovngarde Light and Bold with their license, the logo font, deployed beside the DLL
assets/logo/                  # the logo's SVG and the script that makes its font
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
  `backBufferHeight / 1080` (at least 0.5) times the [font scale](#font-scale).

### Font scale

`[UI] FontScale` ([Configuration](configuration.md#ui)), 80 to 130% in steps
of 5, multiplies the overlay scale (`Overlay::ApplyScale`), which sets
`FontScaleMain` and rescales the style from the unscaled one when the product
changes, from the next frame after the setting applies.

- Almost every length in the overlay is in font sizes, so the setting resizes
  the whole overlay with the text: the settings, chat, trade, and player
  windows, notices, nameplates, the status panel, and the menu frame. The
  user chose this over scaling only the glyphs, which would need every layout
  redone. The cursor's speed ([Input focus](#input-focus)) stays on the
  display's scale alone.
- The range is the user's. At 130% the trade panel's 52.8 font sizes are
  1.7% taller than the display at any resolution (1098 px at 1080p), so the
  panel is placed at the display's top and its bottom is cut; at 125% it
  fits with 12 px to spare at 1080p.
- The settings window's size is set only when it is first shown, so
  `MenuFrame::BeginWindow` scales its last size and keeps its center when the
  font size changes, whether it changed while the window was open or closed
  ([Menu frame](#menu-frame)).
- The DX11 backend does not save the output merger, so the overlay binds the
  swap chain's render target view, draws, and restores the previous targets.
- `ImGuiConfigFlags_NoMouse` and `NoMouseCursorChange` are set unless a window
  takes the mouse, and `imgui.ini` is disabled.

### Backdrop

`Backdrop::Draw` covers the display behind every overlay window (ImGui's
background draw list) with the finished game frame, blurred and darkened;
the [trade window](#trade-window) draws it each frame. The game keeps
running, so the blur is made again each frame from the back buffer as the
overlay hook finds it, HUD and menus included, after `ItemPreview::Update`
has put back what its capture changed.

- It copies the back buffer, reduces it to a quarter in each dimension with
  four bilinear taps per pixel, and blurs that with a 17-tap Gaussian
  (σ 3.5 quarter pixels) horizontally, then vertically, so the blur scales
  with the resolution. The result is stretched over the display at 60% of
  its brightness (`kTint`).
- `Gpu` (`ui/render/Gpu`) holds what the overlay's own GPU passes share:
  the back buffer (`GetFrame`), render targets, pixel shaders compiled at
  runtime by `D3DCompile` (`vs_4_0`, `ps_4_0`, from the
  `d3dcompiler_47.dll` the plugin already links) after a shared `Vertex`
  declaration, and `AddImage` with an opaque or premultiplied blend
  callback. `PassScope` draws full-screen triangles from `SV_VertexID` with
  blending, depth, and culling off, and on leaving puts back the input
  layout and topology, every shader stage but compute, the pixel stage's
  first two views and first sampler, the rasterizer state and viewports,
  and the output merger's target, depth stencil, blend, and depth state,
  since the engine tracks what it bound.
- A multisampled back buffer, a shader that does not compile, or textures
  or states that cannot be created turn the blur off for the session with
  one warning (`UI backdrop blur unavailable: <reason>`); the backdrop is
  then a 40% black fill.
- `Backdrop::Update`, before `ImGui::NewFrame`, frees the copy and the
  reduced textures when nothing drew the backdrop in the last frame.

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
  `Sovngarde OFL License.txt`, and are read from `PlayerHub/fonts/` under
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

#### Logo

The settings window's sidebar is headed by the PlayerHUB logo, drawn from
`PlayerHub Logo.ttf`, a font of two glyphs that `Fonts::GetLogo` returns:
U+E000 is the letters and U+E001 the underline bar. Dear ImGui rasterizes and
caches them at any size with the other glyphs, so the logo stays sharp at every
scale and costs two quads a frame.

- `assets/logo/logo.svg` is the source. `assets/logo/generate.py`, run with
  `uv run --script`, merges its white paths into the letters, cuts its black
  scratches out of its orange bar with skia-pathops, converts the curves to
  quadratics within one font unit at four units per SVG unit, and writes
  `assets/fonts/PlayerHub Logo.ttf` with fixed timestamps. The em, ascent, and
  descent span the logo's height, so the font size in pixels is the logo's
  height; both glyphs advance by its width and share one origin.
- The file is deployed and loaded with the [themed faces](#themed-faces), as
  a font of its own with no fallbacks and no `ExtraSizeScale`.
- `DrawBrand` draws both glyphs at the cursor: the letters in the text color
  and the bar in the [accent](#accent-color), which defaults to the logo's
  `FF9000`. The scratches show the sidebar's panel. The logo starts where the
  navigation items' labels do and fills the width between that inset and the
  same one on the right, the width the connection summary gets. Its height is
  that width over the logo's aspect, taken from the glyph's advance, floored to
  whole pixels because Dear ImGui rounds font sizes; so the right edge can fall
  a few pixels short of the inset and never past it. A `Dummy` of the logo's
  size keeps the layout.
- Without the file, the sidebar shows a text title in the heading face, and
  the log warns.

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

- The accent is `[UI] AccentColor` ([Accent color](#accent-color)), `FF9000`
  by default, given by `Theme::GetAccent` and, at an alpha, `Theme::Accent`.
  It marks what the player can act on or has selected: the border of the
  window's popups, the selected section, a switch that is on, the selected
  and hovered list rows, hovered and pressed buttons, fields, and scrollbar
  grabs, selected text, the chat's player list button and the player manager's tab
  shown, and the fill of the button a view leads to
  (`Controls::PrimaryButton`, 14% darker while pressed). Body text is never
  the accent, so amber warnings stay distinct.
- Text on the accent fill is `Theme::GetOnAccent`: the near-black
  `Theme::kOnState` or the body text color, whichever has the higher WCAG
  contrast with the accent, so a dark accent takes light text.
- `Theme` works out the accent, its text color, and the
  [frame color](#menu-frame) again only when `Config::GetRevision` changes, so
  a call costs one atomic load. These calls and `Scope` are for the overlay
  thread. `Scope` pushes the accent at each accent style color's alpha.
- A state is `Theme::kPositive` (green), `kWarning` (amber), or `kNegative`
  (red): the connection indicator, the picker's warnings, values that are not
  applied, refusals, and save results. Red also fills the button that ends
  what is running (`Controls::DangerButton`, with `Theme::kOnState` text).
  States do not follow the accent.
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
- The file is `PlayerHub/frame/menu-frame.shape` under `Config::GetDirectory`,
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
  whole window, since a window's own leaves out part of its padding.
- Triangulating the 21 concave pieces is most of what a frame costs, about
  30 µs in a Linux benchmark, so `Draw` keeps the triangles of the 4 frames
  it drew last, keyed by everything that shapes them: the window's size, the
  scale, both colors, the font atlas's white pixel, and the draw list's
  anti-aliasing flags and fringe. A window drawn again with the same key
  copies them, moved to its corner; a drag that resizes it triangulates in
  every frame, as before. A frame during which the draw list started its
  vertices over is not kept. Nothing is allocated once the draw list and the
  kept frames have grown.
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
- `MenuFrame::BeginWindow` begins the settings and trade windows the same way:
  centered and sized in font units the first time they are shown
  (`ImGuiCond_Once`; a fixed window is placed every frame), focused on request,
  padded by the frame's inset (plus a font size for trade), with its rounding,
  and with the frame drawn behind the content. A resizable window keeps its
  size and position in its ImGui state storage each frame it is drawn; when
  the font size differs from the kept one, including on the first frame after
  reopening, it is resized by their ratio around its kept center and clamped
  to the display. The kept rect, not the current one, is
  scaled because `Begin` has by then grown the window to the new minimum size.
- `MenuFrame::BeginPrompt` begins the [trade prompt](#prompt) and the
  [display name hint](#display-name-hint) the same way: sized to the content,
  without inputs or focus, centered 6 font sizes from the top under the
  compass, padded by half the frame's inset plus corner beyond the style's
  padding, and framed over the window background at 65% opacity. On the frame
  it reappears it lays out but is not rendered (`ImGuiWindow::Hidden`), since
  ImGui sizes a reappearing auto-resized window from the contents it last
  had and hides only new windows and popups for that frame.
- `MenuFrame::GetMetrics` gives a window those two lengths in pixels to lay its
  content out by. A knot stays inside the square of `corner` at its window
  corner, and cuts across it, so content is clear of it outside that square or
  halfway into it along the diagonal.
- The line color is `Theme::GetFrameColor`, which follows `[UI] BorderColor`
  ([Configuration](configuration.md#ui)) each frame, so a pick in the settings
  window shows at once: `Vanilla`, the default, is the game's `999999` as it
  looks at the 80% opacity of the game's message box over the dark backing,
  `7D7D7D`, and `Accent` is the [accent](#theme) as it looks at 85% over the
  theme's surface color, `DB7D03` for the default `FF9000`, so it follows
  `[UI] AccentColor` too. The backing is the window's own background color,
  and a window's popups keep their accent border with either.
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
- A window gives focus back with `WindowManager::ReleaseFocus(*this)`, or names
  release keys with `Window::IsReleaseKey`: `InputCapture` releases the focused
  window when such a key is pressed, and neither the window nor the game sees
  that press. A release key that edits text, an ImGui key such as Delete or
  Home or a key that types a character on common layouts (letters, digits,
  punctuation, Space, the numpad), goes to a text field instead while one is
  active (`io.WantTextInput` as of the overlay's latest frame), so it still
  edits.
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

### Input language

Goal: switching the Windows input language while the game has focus changes
the layout typed into a window's text field at once, as it does elsewhere in
Windows.

- Typed text comes from Skyrim's `CharEvent`s, which
  `BSWin32KeyboardDevice::Poll` (ID 68782, SE 67472) builds with
  `MapVirtualKeyA` and `ToUnicode`, so they follow the layout of the thread
  that polls. Windows switches the language only for the thread that owns the
  game window. The poll runs on that thread while the game has input, but on
  other threads while a window holds focus (seen in a Windows log, 1.7.104).
  Those threads keep the layout they started with, so a window typed in the
  old layout.
- `InputLanguage::Install`, called from `Overlay::Install`, replaces both
  `call [rip+disp]` sites (+0xEC, and +0x2CB on 1.7.104 or +0x20D on SE, the
  only calls to either import in each executable) with `write_call<6>`, after
  checking that each still reads the user32 import. The replacements call
  `MapVirtualKeyExA` and `ToUnicodeEx` with `GetKeyboardLayout` of the game
  window's thread, found once from the swap chain's window; until that window
  exists they use the calling thread's layout, as the game did. The game's
  menus follow the language the same way.
- When the layout used changes, including the first key, the replacement
  logs it at info with the polling thread, that thread's own layout, and
  the character produced.
- The plugin reads the window thread's layout and never activates one:
  `ActivateKeyboardLayout(…, KLF_SETFORPROCESS)` from the input dispatch
  hung the game on Windows.

## Status panel

The panel sits in the top-right corner, 12 px from both edges before scaling, on
a translucent background, and takes no input. It is drawn only during a game
(`GameSession::IsInGame`) and while `[Advanced] StatusPanel` is on, which it is not
by default; a change applies on the next frame.

| State | Shows | Color |
| --- | --- | --- |
| Not connected, whatever the reason | disconnected | red |
| Connected, no sample yet or timed out | ping without a value | grey |
| ≤ 80 ms / ≤ 150 ms / > 150 ms | ping in ms | green / yellow / red |

While connected, two grey lines follow with the received and sent traffic in
KB/s ([Network](network.md#traffic)), without a value before the first
window. Always shown below: the [plugin time](#plugin-time) in ms, without a
value before the first window.

The panel is a debugging readout, not player guidance. It does not say why a
connection is down or that a display name is missing: those need a sentence
and an action in the settings window, so the messages are on its
`[Connection]` page ([Connection controls](#connection-controls)), and the
[display name hint](#display-name-hint) leads there.

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

## Display name hint

Goal: a player without a display name, who therefore cannot connect
([Network](network.md#sessions)), learns where to set one. Non-goals: a
dismiss control or a timeout: without a name the player cannot play online
at all, so the hint stays until one is applied.

- `DisplayNameHint`, registered after the trade prompt, draws while a game
  is loaded (`GameSession::IsInGame`), the applied `[Connection] DisplayName`
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
sight, so players can tell who is who. Friends' names are in the accent.
Non-goals: a level, health bar, or other detail on the plate (the user chose
the name alone), plates for players without a character in the local world,
plates through walls (the user chose to hide them), and a plate for the
player's own character.

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
  friend's by `PlayerFilters::IsFriend` on the roster's session. Both are
  read again only when the roster's revision (`ChatRoster::GetRevision`) or
  the filters' revision changes.
- `NameplateSync::GetPlates` gives the pass's plates sorted farthest first,
  each with the head node, label, friend flag, fade, and distance as a
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
- A plate's anchor is 30 units above the head node, times the node's world
  scale, projected each frame with `Main::WorldRootCamera`, so it follows
  the head as rendered. `NiCamera::WorldPtToScreenPt3` divides by the
  depth's absolute value, so the layer computes the depth from the
  `worldToCam` matrix's fourth row first and skips a point at or behind the
  camera. The name is centered on the anchor with its bottom there, in
  Sovngarde Bold (`Fonts::GetHeading`, or the overlay's font without it), at
  1.2 font sizes near the player down to 0.8 at the range, rounded to whole
  pixels. It is white, or the accent for a friend, over a black outline of
  four copies offset diagonally by a pixel per 16 px of the overlay's font
  size, at 80% of the plate's alpha. Over the last 15% of the range it fades out; a plate wholly
  off screen is skipped.
- Per pass the cost is one distance per peer with a character, the budgeted
  rays, and one roster and filter revision read; nothing allocates once the
  plates vector has grown. Drawing costs five text draws per plate.

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
  that frame's other notices. Automatic reconnects after a lost connection
  are announced too, and the notice does not name the server: both the
  user's choices. A generation that ended before the stack saw it is not
  announced.
  Every notice is its own toast, a repeat too: the user chose not to merge
  them into one with a count.
- Notices end without a final period, the user's choice. So do the
  [notes](#notes) and a trade row's reason, which share their texts with
  notices: a chat rejection is a note or a notice depending on whether the
  chat is open, and a reason is both the item card's warning and part of a
  notice.
- A notice shows for 2 s plus its length at 15 characters a second, at least
  5 s and at most 10, and fades over its last second, timed by
  `ImGui::GetTime`. Notices keep their time while none is drawn, outside a
  game (`GameSession::IsInGame`); in a game they show in menus too.
- They stack on the right edge, the first one's top at 40% of the screen
  height, the oldest on top, 0.375 font sizes apart, each right-aligned
  0.75 font sizes from the edge, like the chat from the left. Each is a box
  with the theme's surface at 85% and its corners, its text in the theme's
  text and face, wrapped at 22 font sizes less the padding, and a strip in
  the accent on its left edge. The strip and the surface are clipped from
  one rounded box (`Controls::StripedBox`), so the strip follows the box's left corners: a
  separate strip rect would have its rounding clamped to its narrow width.
  Notices have no kinds: the user chose the
  accent for all over a color per success, warning, and info.
- They are drawn in a `Theme::Scope` on Dear ImGui's background draw list,
  under every window, so they take no input and never cover a window the
  player uses; a window over them hides them.

## Chat

Players talk in plain text on three channels. Chat is presentation only. The
wire, the roster, routing, and server rules are in [Network](network.md#chat).

Goal: talk to everyone, to players nearby, or to one player; always see where
the next line goes and change it with a key, a prefix, or a click;
whisper by a few letters of a name, and keep whispering the same player
without naming it again; see who is online and whisper one from a list; tell the channels apart at a glance, and turn each
off and on; see as many lines as the player wants, by dragging the chat larger;
place the chat elsewhere on the screen.
Non-goals: chat
commands (text starting with `/` is sent as written; Tab, the chip, the
`@` prefix, and the player list cover what they did), chat
history for late joiners, a saved log, moving the chat by dragging it (its
[position](#position) is set in the settings window), and
staying open after a line is sent.

| Channel | Reaches | Color |
| --- | --- | --- |
| Nearby | placed players within `chat.nearby_radius` (8192) in the same worldspace, or in the same interior | lime |
| Global | every connected player | gold |
| Whisper | one player | violet |

`ui/chat/` draws and forwards keys; what they mean is in `sync/chat/`, which
has no Dear ImGui and no engine access.

### Send target

`ChatSync::Composer` holds the target (`ChatSync::ChatTarget`): a channel, and
for a whisper a player by slot and name. It starts as Nearby, survives closing
the chat, and is not saved. `ChatInput` shows it as a chip left of the text
field, as high as the field and with its corners, in the channel's color over
an 18% fill of it, 30% under the cursor, naming the channel or, for a
whisper, the recipient, the name cut at 12 font sizes. The empty field's
placeholder points to Tab.

| Input | Effect |
| --- | --- |
| Tab, Shift+Tab | The next or previous of Global, Nearby, and the 3 latest whisper partners, most recent first. A whisper target that is not among them yet stands after Nearby |
| `@` as the first character | Opens the [name list](#name-list) |
| A click on the chip | A menu of Global, Nearby, and the 5 latest partners |
| A click on a sender's name, or the whisper item in a line's menu | A whisper to that player |

- Each row of the chip's menu has a tooltip saying who its target reaches,
  naming the partner for a whisper. `DrawRow` reports the row's
  `ImGuiHoveredFlags_ForTooltip` hover from its selectable, so the whole row
  shows it, not only its text.
- The `@` prefix, after any spaces, is removed from the text once its name
  binds ([Name list](#name-list)), or on Enter, where the chat then stays
  open. `Composer::OnEdited` applies it after every edit, so the chip is
  always where Enter sends. Removing one can uncover another, as in
  `@Ulf @Bera hi`, which is applied in the same edit.
- `ChatRoster` keeps the whisper partners, at most 8, moved to the front by
  every whisper sent or received except one to oneself, and compared by name
  as well as slot, since a player who connects again has another slot.

### Name list

While the text starts with `@`, a list above the field offers the
connected players whose names start with what follows, at most 8.

- `ChatRoster` copies the roster from the transport when its revision changes
  ([Network](network.md#client)) and sorts it by folded name. `Complete`
  returns the player named exactly as typed, then whisper partners, then
  players within Nearby chat ([Network](network.md#nearby-players),
  `Network::CopyNearbyPlayers`), then
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
- With nobody matching, the list is one row saying so, and Enter shows that
  as a failure note and keeps the text.

### Sending

- Enter sends the text to the target and closes the chat; Enter in the empty
  field only closes. With `[UI] CloseChatOnSend = false`
  ([Configuration](configuration.md#ui)), Enter sends and the chat stays
  open with the field emptied, the target kept, and the keyboard back in the
  field, and Enter in an empty field does nothing; Escape is then the way to
  close it. A send that keeps the chat open becomes what Escape returns to, so
  Escape never brings back a line already sent. `ChatInput` reads the setting
  only on such an Enter, so a change applies to the open chat's next one.
  `ChatSync::Send` trims the text, validates it
  (`protocol/ChatMessage.h`, shared with the parser), and
  `Network::PublishChatMessage` sends `ChatSend` with the channel, the
  recipient's slot for a whisper, and a request ID.
- A whisper resolves its player with `ChatRoster::Resolve`: the slot while
  that connection lasts, otherwise the connection that has the name now.
- When nothing was handed to the server, `ChatSync::Send` returns why, and
  the chat stays open with the text and the target and a failure note: the
  recipient is offline, the player is not connected, or the text has
  characters that cannot be sent.
- A rejecting `Result` runs its callback on the transport thread, which
  queues each server rejection (`rate limited`, `not in world`,
  `unknown recipient`) as a sentence. `ChatWindow` takes them every frame: a
  failure note while the chat is open, otherwise, as after Enter closed it, a
  [notice](#notices).
- Up and Down in the field, while no list shows, step through the last 50
  lines sent in this game session, consecutive repeats kept once. A recalled
  line restores its target. Down past the newest returns the text and target
  that were being written.

### Notes

What the chat answers to the player's own input is a note above the field,
never a line, so the history holds only what players said.

- Every note is a failure. `Composer` keeps them as text and replaces them
  on every send. Any edit, a recalled line, a new target, and opening the chat clear
  them; the field's text is kept. Like [notices](#notices), notes end without
  a final period.
- `ChatInput::DrawNotes` draws them where the name list would be, which
  hides them while it shows: a child window as wide as the field with the
  popup's opaque background and border, as tall as its wrapped rows, ending
  one item spacing above the field, in `ChatStyle::kFailureColor`
  (orange-red). The panel takes no input.

### Lines and filters

- `ChatSync::DrainLines` returns received messages. A line carries
  its label and the other player: the sender, or the recipient of the local
  player's own whisper, whose label marks it as sent to the recipient, where
  a received one's marks it as from the sender. Senders are shown as a player label
  ([Appearance and equipment](appearance-and-equipment.md#player-label)).
- `ChatHistory` keeps the latest 100 lines of each channel,
  each stamped with `ImGui::GetTime()` when it arrived, so a busy channel only
  pushes out its own lines.
- A row above the lines has a filter for each channel: Global, Nearby, and
  Whisper. Each is a toggle of its own: a click turns its channel's lines
  off or on, in the open chat and among the closed chat's lines, and changes
  no other filter. All are on at first and any number may be off.
  `ChatHistory` keeps collecting the lines of a channel that
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
- Each filter has a tooltip (`ImGui::SetItemTooltip`) saying which lines its
  channel holds and whether a click shows or hides them.
- A line's label, everything up to and with its colon, takes the channel's
  color and the heading face ([Themed faces](#themed-faces)). Text is the
  theme's, in the text face. Under the cursor a label is underlined; a right click on a line with
  another player, its sender or the recipient of one's own whisper, opens
  its [player menu](#player-menu). One's own other lines have no menu.
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
  short.

### Player list

- The filters' row ends, at the field's column, with a button: a person
  drawn as shapes, from the top of the digits to their baseline as the
  font's `0` glyph has them, and the number of connected players, the local
  one included (`ChatRoster::GetPlayerCount`), or
  an offline label in the muted text color without a roster. It is a chip as the
  filters are (`ChatStyle::DrawChipFrame`): closed, an outline in the
  theme's text color with the 8% fill under the cursor; open, in the accent
  with the chip fill. A click opens the list or closes it. The label is
  formatted only when the number changes.
- The list is a window of its own, half a font size right of the open chat,
  or left of it when the screen has no room on the right, and as high, with 13 font sizes of content, in the chat's menu frame and
  background. `ChatWindow` places and begins it as it begins the chat
  (`BeginFramed`), after the chat, so it has the chat's size from the same
  frame; `ChatPlayers` draws what is inside.
- A search field stands on the filters' row, as high as the filters, with
  a rule under it as far as the chat's is, so the two line up. It has a
  placeholder while empty and keeps names that contain its text, both
  folded as the [name list](#name-list) folds them. Tab stays in the field;
  Enter gives the keyboard back to the chat's field.
- The names follow, a chat line apart, in the text face. Players
  nearby, the ones Nearby chat reaches (`ChatRoster::CopyNearbyPlayers`,
  [Network](network.md#nearby-players)), so in the same interior at any
  distance, come first in the Nearby color, as the chat names a sender in
  Nearby; the
  others follow in a light gray (`kDistantPlayerColor`), between the
  theme's text and disabled text; each group keeps the order
  `ChatRoster` sorts them in (by folded name). The local player is left
  out; the chip's count still includes it. A name wider than the list is
  cut at its edge. With nothing to list it shows one row saying why: no
  name matches the search, nobody else is online, or there is no roster.
- A name acts as a sender's name does in the lines: under the cursor it is
  underlined and the cursor is a hand, a click whispers the player, and a
  right click opens its [player menu](#player-menu). The keyboard then
  returns to the chat's field.
- A muted or vanished player has a mark at the list's right edge, in the
  muted text color, three quarters of a font size square: a speech bubble
  struck through for muted, an eye struck through for vanished, both when
  both. Hovering them names the state. A vanished player within Nearby chat is still
  nearby, since its Nearby lines still arrive.
- A button as wide as the list stands at its bottom, below the names, and
  opens the [player manager](#player-manager).
- Only the button closes the list. A chat closed by Escape, also from the
  search field, by Enter, or by a menu that opens by itself shows the list
  again, with its search, when it opens. Closing the list empties the
  search, and nothing of the list is saved.
- The wheel over the list scrolls the list only: `ChatHistory::DrawLines`
  leaves the wheel to it while `ChatPlayers::IsHovered`.
- Closed, the list costs the count. Open, `ChatRoster::CopyPlayersIfChanged`
  copies the roster only when its revision changes, and the search is
  matched then, on an edit, and when the players nearby change; those are
  copied every half second (`kNearbySeconds`); `ImGuiListClipper` draws
  only the rows in view; and
  its frame is copied from the [menu frame](#menu-frame)'s kept triangles,
  so the list adds a few microseconds to the chat at any number of players.

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
  rate. The size stops at the limits and at the room from the chat's
  [position](#position) to the right and top edges (`GetUnits` in
  `ChatWindow.cpp`), and, while it is open on the right, the
  [player list](#player-list) beside it. A
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

### Position

- `[UI] ChatPositionHorizontal` and `ChatPositionVertical`
  ([Configuration](configuration.md#ui)) place the lower left corner of the
  open chat's frame (`ChatPosition::Get`) at a percentage of the screen's width
  from its left edge and of its height from its bottom edge, so `0` and `0` is
  the screen's lower left corner and the place is the same share of any
  resolution, independent of the font scale. The defaults, `1` and `8`, keep
  the chat where it was placed in font sizes before, near the left edge and
  above the magicka bar: at 1080p and the default font scale about 7 px
  further right and 2 px lower.
- The chat stays at its position: its [size](#size) is limited to the room
  from that corner to the right and top edges, rather than the chat being
  moved back onto the screen. So a drag on the right edge grows the chat to
  the right up to the screen's edge. Only a chat at its minimum size, which the room does not hold, is
  moved until it fits (`KeepOnScreen`). The recent lines are placed from the
  same corner with the open chat's size as last measured, so they keep their
  column when the chat opens.
- The [player list](#player-list) goes right of the chat when the chat at its
  configured size and the list fit there. Otherwise it goes left when there is
  room on the left, so the chat keeps its size; without that room it stays
  right and the chat narrows for it (`ChatWindow::ChooseListSide`). The side is
  chosen from the configured size, not the narrowed one, so it does not change
  back and forth.
- The settings window shows both as percentage sliders; Defaults on `[UI]`
  resets them. The chat moves when a slider is released.
- The player also moves the open chat by dragging any part of it that takes
  no click itself: the frame's left and bottom strips, the gaps in the
  filters' row, and the lines away from senders' names. The top and right
  strips stay the [size](#size) grips. `ChatPosition::Grab` runs after every
  item in the window, so `ImGui::IsAnyItemHovered` covers the filters, the
  players chip, the target chip, the field, and the scrollbar; a sender's
  name takes its click without being an item, so a click that picked one
  (`ChatHistory::DrawLines`) starts no move, and neither does one while a
  menu is open or over a resize grip (`ChatSize::Grab`). Moving starts past
  Dear ImGui's drag threshold, so a plain click, which only takes the field's
  focus until the button is released, moves and saves nothing. While moving,
  the cursor is the move one.
- During the drag the corner follows the cursor by the whole pixel and stops
  where the chat at its configured size, not the size the room leaves it,
  touches a screen edge (`ChatPosition::Follow`). A drag never shrinks the
  chat, then; one grabbed from a slider's position whose room had narrowed it
  first grows back to its configured size within the screen. `GetUnits`
  allows half a pixel, so a chat against the right or top edge, whose room is
  exactly its size, keeps that size. The player list's side is chosen anew
  each frame, so it goes left as the chat nears the right edge.
- Releasing the button, or the chat losing focus mid-drag
  (`ChatPosition::Release`), stores the nearest whole percentages, applies
  them with `Config::Apply` only if they changed, and logs them. A nearest
  percentage whose corner would leave the chat less room than the drag did is
  rounded toward the lower left corner instead, so a chat dragged against the
  right or top edge keeps its size. The chat therefore settles up to half a
  percent from where it was dropped, about 10 px across and 5 px up and down
  at 1080p, or a whole percent beside the right or top edge.

### Window and input

- `[Controls] OpenChat` ([Configuration](configuration.md#controls)), Enter
  by default, during gameplay opens the chat and focuses its field; keypad
  Enter opens it only when bound. The key only opens: the chat closes by
  Enter and Escape as below. The whisper item in the
  [player menu in the world](#player-menu-in-the-world)
  opens it too, with that player as the send target.
  Typing goes only to the chat; the character does not move or look around, and
  keys already held keep acting until released.
- Escape closes an open menu, else the [player manager](#player-manager), else
  hides the name list, else discards the text typed since opening
  or since the last line sent ([Sending](#sending)) and closes the chat; an open [player list](#player-list) shows again
  when the chat opens. Every key,
  console and menu keys included, goes to the chat while it is open, but a
  menu that opens by itself, such as dialogue an NPC starts or a loading
  screen, closes it and keeps the text.
- The chat uses the mouse, with the cursor hidden until the mouse moves
  ([Input focus](#input-focus)), so a player who only types sees none. It
  appears in the middle of the chat. After any click the keyboard returns to
  the field, except one in the player list's search field; a click outside
  the chat and the list does nothing.
- Open, it sits at its [position](#position), with the
  filter row, the lines at the chat's [size](#size), and the input row,
  inside the [menu frame](#menu-frame) over a 60% background. The frame adds half of its
  inset and corner to the padding on
  every side, 19 px at 1080p at the default thickness, which keeps the content
  clear of the knots. The wheel scrolls 3 lines per notch and Page
  Up/Down a page; under the shown cursor the lines scroll as Dear ImGui does,
  and over the player list the list scrolls instead.
  New lines scroll down unless the history was scrolled back.
- Closed, the newest lines (at most 6) show for 10 s on a faint background and
  fade out over the last 2; with nothing recent nothing is drawn. They follow
  the filters, whispers included, so turning Global off is how a player
  keeps global chat off the screen. They have no
  frame: its corners alone are taller than a line or two. Their box is in from
  the chat's corner by the frame's padding, so the lines stay in the same
  column when the chat opens.
- The open chat and the recent lines are each drawn in a `Theme::Scope`
  ([Theme](#theme)), so the chat has no colors or sizes of its own beyond
  `ChatStyle`'s channel and failure colors: the field is the theme's white
  tint with 6 px corners, the scrollbar its thin grab, the chip's and a
  line's menus its opaque popups with the accent border, and their hovered
  and selected rows the accent. The name list is a child window
  with the popup's background, so it looks the same. Text is Sovngarde Light
  ([Themed faces](#themed-faces)).
- The filters, the lines, the chip, the list, and the recent lines start at one
  column, and the player button, the rule, the scrollbar, the list, and the
  field end at one.
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

## Mute and vanish

Goal: a player can stop seeing another player without leaving: mute hides
what it says, vanish removes its character from this world. Either is chosen
from a [player menu](#player-menu), in the chat's lines, the
[player list](#player-list), or the world, shows as a mark in the list, can be undone
there or in the [player manager](#player-manager), also while the player is
offline, and lasts across games on the same server. Non-goals: telling the
player, a mutual vanish (a vanished player still sees the one who vanished
it), muting or vanishing by name, a vanished player's Nearby chat (mute
covers chat), and admin moderation.

- `PlayerFilters` (`sync/filters/`) holds the current server's entries by
  session ID: a name, `friended` ([Friends](#friends)), `muted`, and
  `vanished`, each independent of the others. It reads them from
  `Network::PlayerFilterStore` for `Network::GetServerAddress` and again
  whenever the connection generation changes, so a session is never looked
  up on another server's list ([Network](network.md#player-filters)). A
  revision, the sum of its own change count and the generation, tells
  readers when to copy again; it is two atomic loads.
- `SetMuted` and `SetVanished` change one flag and save the entry, without a
  [notice](#notices): the menu item, the list's marks, and the hidden lines or
  character already show the change. `SetVanished` then publishes it
  ([Network](network.md#vanish)). A player without a session cannot be
  either. At 1024 vanished players on a server another vanish is refused
  with a notice giving the limit. A failed save posts a notice that the change lasts until the game exits.
- The [player menu](#player-menu) ends with a separator, a mute or unmute
  item, and a vanish or unvanish item, each with a tooltip saying
  what it does and that the player is not told. The local player
  (`ChatRoster::GetLocalSession`) and a player without a session get none.
- Muting hides the player's lines on every channel, whispers included, in
  the open chat and the closed chat's recent lines, and what it said before
  the mute too; unmuting shows every line still held. `ChatHistory` keeps a
  `muted` flag per line from the sender's session (zero for the local
  player's own lines), set when a line arrives and recomputed
  for all lines only when `PlayerFilters`' revision changes. A muted line
  marks no filter unread. A muted player's whisper does not become a whisper
  partner (`ChatSync::DrainLines`).
- Vanishing is the server's: the client stops receiving the player
  ([Interest and delivery](interest-and-delivery.md#subscriptions)), so its
  remote actor leaves as for any `EXCLUDED` peer and returns, if in range,
  when the vanish is undone. It does not hide the player's chat.

### Player manager

Where the friends, muted, and vanished players of the current server are
listed, offline ones included, and undone.

- `ChatPlayerManager` draws it and `ChatWindow` places and begins it
  (`BeginFramed`) after the chat and the player list, or alone, in the menu frame
  over 90% of the theme's background, with 26 × 22 font sizes of content.
  It opens in the middle of the screen (`ImGuiCond_Appearing`) and can be
  dragged by any empty part while it is open; every opening starts in the
  middle again. The open chat's windows
  have `ImGuiWindowFlags_NoBringToFrontOnFocus`, so a click in the chat or
  the list leaves it in front. The list's button while it is open brings it
  to the front.
- It holds a title in the heading face with a close button at the right,
  a tab each for friends, muted, and vanished players as chips
  (the one shown in the accent with the chip fill, the others outlines that
  brighten under the cursor), and a rule. Every opening shows the friends tab;
  nothing of the window is kept once it closes.
- A tab with players has a search field, with a placeholder, that keeps
  the names containing its text, folded as the [name list](#name-list)
  folds them; an empty tab has none. The search stays across tabs and
  empties when the window closes; Tab stays in the field. Then a row per
  player of the tab, the connected ones first, each group sorted by folded
  name: a dot of 0.4375 font sizes,
  green (`kOnlineColor`) while the player is connected and in the muted
  text color otherwise, whose tooltip tells which; the name, cut before the
  button, the roster's while the player is online and as kept otherwise;
  and a button that removes the friend, unmutes, or unvanishes, as the menu
  does, and removes the row at once. An empty tab shows a line saying it
  has nobody on this server, and a search matching nobody a line saying so.
  `ImGuiListClipper` draws only the rows in view.
- A name acts as a sender's name does in the chat's lines, on every tab:
  under the cursor it is underlined and the cursor is a hand, a click
  whispers the player while it is online, and a right click opens its
  [player menu](#player-menu) (`##ManagePlayersMenu`), offline too. An
  offline player has no slot, so its menu has no race and level line and
  whisper is disabled. Whisper sets the chat's target as the list's
  does, and the window stays open. The chat counts the menu among its open
  menus (`ChatPlayerManager::IsMenuOpen`), so Escape closes it first and
  the field leaves the keyboard to it.
- It copies the roster and `PlayerFilters` only when their revisions
  change, and rebuilds the tab's rows then and when the tab changes.
- Escape closes it before the chat, from any field. Its close button closes
  it alone. Closing the chat, by Escape, Enter, or a menu that opens by
  itself, closes it too, and it does not show again until the list's button
  is pressed.
- `[Controls] ManagePlayers` ([Configuration](configuration.md#controls)),
  `O` by default, during gameplay opens it alone, without the chat (the
  user's choice): the chat's window takes focus and `ChatWindow::OnFocusKey`
  marks it manager-only (`managerOnly`, atomic, as the input thread writes
  it). The cursor shows at once in the middle of the screen
  (`HidesIdleCursor` false), the recent lines show as with the chat closed,
  and the key only opens it. Escape closes an open menu, else the window,
  and its close button closes it; either gives input back to the game.
  A whisper, by a name's click or its menu, closes it and opens the chat
  with that player as the target, as a whisper from the world does. The
  chat's key does nothing while it is open. The wheel over it scrolls it and not the chat's lines
  (`ChatPlayerManager::IsHovered`). After a click the keyboard returns to
  the chat's field, except in its search field.

## Friends

Goal: a player can keep other players to find again: added from a
[player menu](#player-menu), listed with whether they are online in
the [player manager](#player-manager), kept across games on the same server,
and announced by a [notice](#notices) when they connect. Friendship is one way: the
player is not told and need not add back. Non-goals: requests or mutual
friendship, a mark for friends in the player list or chat lines (the
user's choice: only the menu and the tab show it), a notice when a friend
leaves, friends across servers (a session ID names a player on one server),
and a limit, since nothing about friends is sent.

- `SetFriend` sets `friended` and saves the entry as `SetMuted` does,
  without a notice. A friend may also be muted or vanished; removing
  the last of the three drops the entry.
- The [player menu](#player-menu) has an add or remove friend item right
  after whisper, with a tooltip on what it does. The local player and a
  player without a session get neither, as for mute and vanish.
- `PlayerFilters::CollectArrivedFriends`, called by the
  [notice stack](#notices) every frame, copies the roster only when its
  revision changes (`Network::CopyRosterIfChanged`) and keeps the sorted
  session IDs of the previous copy. A roster without the local player,
  before a connection's first `Roster` or after a disconnect, clears them.
  The first roster with a new local slot is a connection's first and lists
  who was already online, so it announces nobody; after it, each friend
  whose session was not in the previous copy gets a notice that it is
  online. A muted or vanished friend is announced too. A friend
  who leaves and comes back between two copies is not announced; one who
  reconnects later is announced again.
- The same pass refreshes the kept name of any entry, friend or not, whose
  player appears with another name, the first roster included, and saves
  it, so an offline row shows the name last seen.

## Player menu

Goal: wherever another player's menu opens, a right click on its name in
the chat's lines or the [player list](#player-list) or Activate on its
character ([below](#player-menu-in-the-world)), it looks the same and says
who the player is: its name, race, and level, then whisper,
[trade](#trade-window), [friend](#friends), and
[mute and vanish](#mute-and-vanish) items. Non-goals: the real name of a modded
race the local game lacks (a race's name is not on the wire; the race the
local game resolves it to is named, as on its character), and following a
level-up while the menu is open.

- `ChatPlayerMenu` is the menu's content. `ChatHistory`, `ChatPlayers`,
  `ChatPlayerManager`, and `WorldPlayerMenu` each hold one, call `Open` with the roster entry when
  they open their popup, and `Draw` inside it, which reports a whisper for
  each to handle as before.
- It starts with the roster's name in the heading face (`Fonts::GetHeading`)
  at the text size and in the accent color (`Theme::GetAccent`), then a
  line with the race and level in the muted text color: the level alone
  without a race name, the race alone at level zero, and no line without
  either. A separator and the items follow. Whisper is disabled for a
  player without a slot, which only an offline
  row in the [player manager](#player-manager) opens the menu for.
- `Open` asks `PlayerDetails` (`sync/chat/`) for the player's slot,
  replacing any earlier request. The next pass serves it on the game thread
  ([Sync pipeline](sync-pipeline.md#frame-sequence)): a peer with an actor,
  which receives every appearance revision, is answered from its latest
  appearance; any other player, the local player included, is asked of the
  server with `Network::RequestInspection`
  ([Network](network.md#inspection)), since an appearance kept after a peer
  left range may be old. Without a connection the answer is empty.
- The race is `AppearanceSync::GetRaceName`: the full name of the race the
  appearance's race resolves to in the local game, with the
  [fallback](appearance-and-equipment.md#resolution-and-fallback) a spawned
  character gets. The level is `Appearance.level`, zero before the player
  sent an appearance.
- The local answer also says whether the player's character is near
  enough to trade (`Details::inTradeRange`, [Trade](trade.md#invitation)); a
  server answer never is, since a player without a character is not near.
- `PlayerDetails` keeps only the latest request and its answer, under one
  lock; an answer for another slot is dropped. While the answer is due the
  menu reads it once a frame and holds an empty line of the text's height,
  so the menu does not grow under the cursor; after
  `kDetailsWaitSeconds` (2) of frame time, each frame counting at most
  0.1 s, it stops waiting and shows the name alone, as for a reply the
  server's rate limit dropped. The answer is read once.

## Player menu in the world

Goal: activating another player's character, or the mount it rides, with
Skyrim's Activate key (E by default) opens that player's
[player menu](#player-menu) at the crosshair. Non-goals: another crosshair
prompt (Skyrim's own prompt and name stay) and gamepad navigation.

- `RemoteActorActivation` (`runtime/`) is a `TESActivateEvent` sink on
  `ScriptEventSourceHolder`, registered once data is loaded. Remote actors
  keep their blocked activation, so Skyrim starts no dialogue, but the event
  is still sent ([Remote actor](remote-actor.md#spawn)). It keeps the local
  player's activations of a registered remote actor
  (`ActorRegistry::IsRegistered`) and stores the actor's form ID with
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
  with its corner there, over the crosshair. It holds the
  [player menu](#player-menu); the chat stays closed.
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
  first, draws while a trade is requested, invited, or inviting: a framed
  panel at the top center, 6 font sizes down, under the compass. It has
  no inputs and never takes focus to show.
- Two lines, each measured before it is drawn and centered on the wider
  one, `kHintGapFonts` (0.5) font sizes apart beyond the item spacing: the
  message, then the key hints and the seconds left.
- Invited: the inviting player's name in the accent and the invitation,
  then an accept and a decline hint, each with its configured key's label
  ([Configuration](configuration.md#controls)), and the seconds left.
  Requesting and inviting look the same, so the prompt keeps its size when
  the server's invitation arrives: that the other player, named in the
  accent, has yet to answer, then a withdraw hint with the decline key and
  the seconds left, which stay at the whole `TradeSession::kInviteSeconds`
  while requesting. The seconds take the width of the longest count, so the
  centered panel does not shift as they lose a digit. An unbound action's
  hint is left out; the invited player then answers from the player menu.
- `IsFocusKey` takes `AcceptTrade` while invited and `DeclineTrade`
  while invited or inviting, and nothing otherwise, so the keys stay the
  game's. While requesting there is no trade ID to cancel yet, so the
  withdraw hint shows but its key stays the game's. The key that gave focus answers, and the prompt gives focus back
  in the same frame, so neither the game nor another window sees it.

### Window

- `TradeWindow` takes focus with `TakeFocus` once the view is open, also
  from the chat or the player menu, and takes the whole display: the
  [backdrop](#backdrop) blurs and darkens the game behind every overlay
  window, and a click on it does nothing. The cursor shows. A window kept from focus
  by a game menu retries each frame and gives the trade up with `FAILED`
  after `kFocusWaitSeconds` (2) of frame time, each frame counting at most
  0.1 s; one that had focus and loses it cancels with `MENU`.
- The trade itself is a framed panel of 56 × 52.8 font sizes, the same share
  of the screen at every resolution, which cannot be moved or resized
  (`MenuFrame::WindowLayout::fixedPosition`). To its right, 2 font sizes
  apart, as the inventory menu shows its item (the user's choice), the
  [preview column](#preview-column) is 28 font sizes wide, less
  on a narrow display while at least 12 fit within a 1 font size margin,
  and left out otherwise. Panel and column are centered together, and the
  panel vertically; the column is as tall as the panel.
- The title names the other player, in the accent; the window has no close
  button, Cancel and Escape close it. The left column holds the player's
  items, and the right column the peer's offer above the player's own, each
  a grid, the order Path of Exile stacks them in.
- Each offer is a grid of 5 × 3 square cells, one line per cell in offer
  order, row by row; the player cannot move items in it (the user's choice,
  as Steam's trade does). An offer holds at most 15 lines
  ([Trade](trade.md#offers-and-acceptance)), so every line is in view and
  the grid has no scrolling; `TradeWindow.cpp` asserts that the grid holds
  `kMaximumTradeItems` at compile time. The cell is the largest whole
  pixel size up to 7 font sizes, at least 5 (`kMinimumCellFonts`, the
  user's choice), that fits both grids' rows in the panel's height and
  leaves the item list 20 font sizes. In the fixed panel the width limits
  it, leaving the item list its 20 font sizes: about 103 px at 1080p
  (6.4 font sizes), estimated from the style's sizes. The 56 × 44 panel
  before it held the cells at their 5 font size minimum (80 px, measured
  headless), so the panel was made 20% taller (the user's choice).
- Each cell is its own borderless well, with a quarter font size gap
  (`kCellGapFonts`, rounded to whole pixels, at least 1) to its neighbours.
  The gap is taken out of the cell's pitch, so the cell size above and the
  grid's footprint are unchanged; the item fills the whole well (the user's
  choice).
- A cell shows the item's [thumbnail](#3d-preview) on its background (light
  blue-grey for an enchanted item, as Path of Exile tints magic items, else
  the empty well's color, `kItemColor` = `kWellColor`, the user's choice)
  and the count above one in its top left corner, in white with a dark
  shadow. The background must be opaque, since the still is captured on
  it. While thumbnails are off (`[UI] ItemPreview`) or unavailable, the
  cell shows the name instead, in dark text on an enchanted item's
  background and the style's text color on the well;
  while one is captured, only the background. Hovering a cell frames it in
  the accent and shows the item in the preview column at once.
- The player's offer: the lines the other game confirmed; a clicked item appears
  only once confirmed. Clicking a cell that offers one item, also one of a
  larger stack, removes its line at once (`TradeSync::SetOfferCount` with 0)
  (the user's choice).
  Clicking a cell that offers more opens a popup with the name, a count field
  with steps of one (clamped to what the player holds) and the count held,
  and a remove button; the field is left out if the player comes to hold only one while
  it is open. Stepping down from 1, or entering 0, removes the line and
  closes the popup (the user's choice); typed text applies only on Enter or
  focus loss (ImGui 1.92.9), so a 0 on the way to another count does not.
  Escape closes the popup, and it closes when its line leaves the offer.
- The peer's offer: names as the local game resolves them; lines it cannot
  receive are left out. Each line must be inspected, as in Path of Exile: a
  line the cursor has not been over is shaded dark, and Accept stays
  disabled until none is left, with a hint after the grid's heading
  counting the lines left. Inspecting is local to the window and takes effect in
  the frame the cursor reaches the cell, so moving the cursor across the
  grid inspects every cell it passes (the user's choice). `TradeSync` keeps a line's `PeerLine::id` while the peer offers the same
  item and count, so only a line the peer adds or changes needs inspecting
  again; the inspected ids last for the trade.
- While the own offer holds 15 lines (`kMaximumTradeItems`), a warning-colored
  mark saying it is full follows the own grid's heading label, before the
  accepted state (the user's placement).
- While a player has accepted, its grid is tinted green with a green frame,
  and its heading says it accepted, so the state does not rest on color
  alone.
- The player's items: a search field (case-insensitive, by name) and a
  category (all, weapons, apparel, ammunition, potions, ingredients, books,
  scrolls, soul gems, keys, miscellaneous), matched again
  only when either or the inventory changes; rows clipped to the visible
  ones, with counts above one. A click on a single item offers it. While
  the offer holds 15 lines, rows not yet offered are disabled, with no
  notice, since the full mark on the own grid's heading shows why; offered rows,
  hovering, the search field, and the category stay usable (the user's
  choice). A click
  on a stack opens a popup with the name, a count field focused with its
  text selected, steps of one, clamped to 1 to what the player holds, and
  the count held, and a button: it starts at 1 and offers, or for a stack
  already offered starts at the offered count and updates it. Only the button or
  Enter sends the count (`TradeSync::SetOfferCount`); Escape, also while
  typing, and a click outside close it without a change, and it closes when
  the inventory changes, since its row may then be another item. Right
  clicks do nothing (the user's choice, for now). Offered rows are
  highlighted; rows that cannot be offered, also those the other game
  [cannot receive](trade.md#offers-and-acceptance), are disabled. A hovered
  row, also a disabled one, shows its item in the preview column at once.
- The footer holds only Accept, disabled while blocked, uninspected, or
  accepted, and Cancel.
  Acceptance shows on the grids alone, and a change taking it back shows as
  the tint going and the changed line shaded. `TradeSync`'s reason Accept is
  unavailable (nothing offered, the offer sending, a report awaited) is the
  disabled button's tooltip rather than a line, since it mostly
  lasts a round trip after each edit and would flicker there. Escape, when
  no field or popup claims it, cancels as Cancel does.
- A trade that ends releases focus; a window that cancelled waits for the
  view to close without taking focus again.

### Preview column

The vanilla inventory's item display, right of the panel (the user's choice):
the item's model above a card with its numbers. It replaces the item
tooltips; the only tooltip left in the window is Accept's blocker.

- It shows the item last hovered in the item list or either grid, and keeps
  it while the cursor is over the backdrop or the panel's empty parts.
  While no item is shown, from the window's opening to the first hover,
  the square is empty and the card is left out. A row of the player's is
  found again by form and name when the inventory changes, since rows can
  move, and the column empties when it is gone or when the peer takes the
  shown line out. Hovering a
  peer cell also [inspects](#window) it.
- The model's square is the column's width, leaving the card room for at least 12
  font sizes, so 28 font sizes square on most displays. It shows the
  item's [model](#3d-preview) floating over the backdrop, as the inventory
  menu does, spins it slowly, and turns it while dragged with the left button. With
  `[UI] ItemPreview` off it stays empty.
- The card is a framed window under the square (`ui/trade/ItemCard`) laid
  out as the vanilla inventory's item card, every line centered. As in the
  inventory menu (the user's choice), its top stays put and its bottom
  border follows the content: `MeasureItemCard` lays the card out without
  drawing before the window begins, and the window is that tall, up to the
  panel's bottom, where a longer card scrolls. The first and last lines
  keep clear of the frame's corner knots (`MenuFrame::Metrics::corner`);
  the sides keep the card's 0.6 font size padding. The content scrolls in
  a child exactly the content area's size, because a window clips its
  content only half its padding from its edge: lines scrolled past are cut
  at that margin, and a card scrolled to its end keeps the same space
  under its last line as one that fits. Whether the card scrolls comes from
  the measurement, not from ImGui: a card that fits gets a child without a
  scrollbar, since ImGui truncates window and child sizes to whole pixels
  and could otherwise find the content a pixel too tall, and its scrollbar
  would narrow the text into more lines that keep it shown. A scrolling
  card lays its text out at the content width less the scrollbar. It shows the item's
  [details](trade.md#item-details):
  - the name in capitals in the heading font at 1.45 times, wrapped onto
    more lines when needed, with the count in parentheses after it for more
    than one, as the vanilla item list writes counts, over a rule the card's
    width.
    Capitals are made for ASCII, Latin-1, Latin Extended-A, and Cyrillic;
    other letters are kept;
  - in amber at 0.9 times, why a row cannot be offered (a quest needs it,
    the peer cannot receive it, and the other [offer blocks](trade.md#offers-and-acceptance)),
    under the rule and before the stats, so it is read first (the user's
    choice);
  - damage or armor rating of one item, then weight and value of the
    whole count it shows (held for an item list row, offered for a grid
    cell, held again once a stack leaves the offer), so a stack and a
    single item read the same: each a gray label at 0.95 times before its
    value in the heading font at 1.75 times, spaced 1.6 font sizes apart
    down to 0.8 and wrapped when they still do not fit;
  - each effect as the game words it in gray with every number (digits
    with single `.` or `,` between them) white in the heading font, as the
    game shows magnitudes; an undiscovered ingredient effect is darker
    without highlights; then the charge, current over maximum, in the same
    style (the user kept it as text rather than the vanilla meter);

  The card shows no equipped, favorite, or enchanted note: the user
  dropped equipped and favorite, and an enchanted item is told by its
  effects and its cell.

## Settings window

Players change `PlayerHub.ini` ([Configuration](configuration.md)) in game and
see the result at once. The game is not paused; the world keeps running and
peers keep seeing the player.

- `[Controls] OpenSettings` opens it during gameplay as a focus key and closes
  it again as a release key ([Input focus](#input-focus)); a changed key
  applies once bound. The close button at the page's top right or Escape also
  closes it; Escape first goes to an active text field, open combo or list, or
  a [key binding](#key-bindings) waiting for a key. Any menu taking input
  closes it too.
- Every opening starts from the live configuration; closing discards unapplied
  outfit edits and text still being typed. The section that was open stays
  open, except that an opening with no display name applied shows
  `[Connection]` with the keyboard in its field.
- Centered at 48 × 35 font sizes the first time it opens in a game session,
  and at least 40 × 20. Later openings keep the position and size it was
  closed with; they live in ImGui's window state only, so a restart centers it
  again. In the [theme](#theme) and the [menu frame](#menu-frame) over a 96%
  background, without a title bar:
  it is dragged by any empty part, resized from its lower corners and its
  edges, and uses the mouse. The sidebar and the page fill what the frame's
  lines enclose. The frame's backing covers ImGui's resize grip, and the line
  ImGui draws along a hovered or dragged edge is transparent
  (`ImGuiCol_SeparatorHovered` and `ImGuiCol_SeparatorActive` around `Begin`),
  so resizing shows the resize cursor only.
- A sidebar 11.5 font sizes wide, on a faint panel, lists one item per file
  section in schema order under the [logo](#logo): `[Connection]`, which
  opens first and is the display name beside the
  [connection button](#connection-controls) above its other settings, then
  `[DefaultOutfit]`, `[UI]`, `[Controls]`, and `[Advanced]` (the user's
  order). `FormatSectionTitle` gives `[UI]` and `[Controls]` titles other
  than their keys. The
  open section's item has an accent tint and, like a [notice](#notices), a
  strip in the accent 0.2 font sizes wide down its full height on its left,
  clipped from the item's rounded box (`Controls::StripedBox`). The
  connection status stays at the sidebar's bottom in every section
  (`ConnectionControls::DrawSummary`).
- The open section's title and the group names inside a section are in the
  heading face; everything else is in the text face.
- The page beside it shows the open section: its title, the section's comment
  as one muted line where the schema has one, and its settings. Only the
  settings scroll, with the scrollbar along the frame's line; the title and
  the footer stay in view. The close button and the end of the settings of a
  section without a footer stay out of the knots' squares.
- Widgets come from the same `Config::Describe` visitor that reads and writes the
  file, so a new setting appears without UI code. A row is the label the
  schema gives the setting
  ([Schema](configuration.md#schema)), and, in the right 58% of the row,
  its control: a switch at the right edge for a bool, a slider for an
  unsigned value or a percentage, a text field for a string, an item picker for a form, a combo for a choice, a button showing
  the key for a key ([Key bindings](#key-bindings)), and swatches for a color
  ([Accent color](#accent-color)). Group comments are small muted
  capitals above their rows, as on `[DefaultOutfit]`, `[UI]`, and
  `[Advanced]` ([Configuration](configuration.md#settings)).
  `[Connection] DisplayName` is drawn beside the
  connection button instead of as a row
  ([Connection controls](#connection-controls)), and a `Layout` setting, as
  the chat's [size](#size) is, is not drawn at all.
- A row the schema shows only while a value is true (`ShowNextIf`,
  [Configuration](configuration.md#schema)) is left out while it is false, and
  its text in red is dropped, so it does not come back red. `[Connection]` so shows `ServerEndpoint` only while `CustomServer`
  is on ([Official and custom server](configuration.md#official-and-custom-server)),
  as a field without a label, spanning the row from the labels' left edge,
  with a placeholder while empty: the switch above it already names it. The official server's URL is never drawn.
- A setting's comment is not drawn; it is the tooltip of the row's label, and
  of a text field while its value is invalid.
- A number's slider runs from 0 to its maximum and shows the value. A drag
  or a typed value is rounded to the nearest step the schema gives, `100`
  for `ViewRadius` and `1` for the others; a value from the file off the
  step shows as it is until the slider moves. Where
  the schema names zero ([Configuration](configuration.md#schema)), it shows
  that name at 0: the name is the slider's format, which Dear ImGui draws as
  it is when it has no conversion. Ctrl and a click turn it into a field, and
  a typed value is clamped to the range.
- A percentage's slider runs between its limits and shows the value with a
  percent sign. Ctrl and a click turn it into a field, and a typed value is
  clamped to the limits; a dragged or typed value is rounded to the nearest
  step from the minimum.
- Edits in every section but `[DefaultOutfit]` take effect and are saved as they
  are made, with no button: a switch, combo, or swatch at once, a key when
  it is bound, a number when its slider is released, so a drag does not save
  the file in every frame, a percentage when its slider is released, so the
  frame does not resize the window's content under the cursor mid-drag, a
  color from the picker when it is released or its hex field's
  editing ends, so a drag does not save the file in every frame, and
  text when editing ends, on Enter, on a click
  elsewhere, or on picking another section, so a half-typed endpoint never
  reconnects. Escape in a text field puts
  back the text it had when editing started. Text that does not parse turns red
  and leaves the setting at its last valid value, while other edits still
  apply, and a red line under the field says that it is invalid: the display
  name's under the [connection bar](#connection-controls), the
  `ServerEndpoint`'s directly under its field, wrapped at the page's width. A
  red line appears below the settings only while the file could not be
  saved.
- `[UI]`, `[Controls]`, and `[Advanced]` have a footer with one Defaults
  button at its right, enabled while a setting the page shows differs from
  the built-in one (`Config::GetDefaultConfig`); the red line shares the
  footer, at its left. A click restores the page's settings at once and
  applies them, without a confirmation; a listening
  [key binding](#key-bindings) stops listening, as on any click. `[UI]`
  keeps the chat's [size](#size), which the page does not show.
  `[Connection]` has no footer, so its settings are reset by editing them or
  removing the line from the file.
- `[DefaultOutfit]` keeps its edits until Apply, since an outfit is put together
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
  differ and no slider or color picker is active, and Apply does
  the same with the draft's `defaultOutfit`.
- The page is drawn before the sidebar, in one `Config::Describe` pass that
  also collects the section names the sidebar lists. A picked section
  therefore shows from the next frame, after a text field edited until the
  press has seen its editing end.

### Accent color

The player picks the [theme](#theme)'s accent on the `[UI]` page, so the settings
window, the open chat, and their lists can match a modpack's look.
`[UI] AccentColor` ([Configuration](configuration.md#ui)) holds it, and
`Config::SettingsVisitor::Color` passes the swatches
`Config::UISettings::kAccentSwatches` with it.

- The `AccentColor` row has, across the control, the eight swatches, amber
  `FF9000` (the default), grey `909090`, gold `F2C14E`, red `E5484D`, pink
  `E0559C`, violet `9B6CF0`, blue `4F8EF7`, and teal `2EC4B6`, then a custom
  swatch. The grey is the one whose `Accent` [frame](#menu-frame) is the
  `Vanilla` frame's `7D7D7D`, so the widgets can match the game's grey
  frame. Each is a square no wider than a frame is high, shrinking so the
  nine keep at least a quarter of a font size between them; the first starts
  at the control's left edge and the custom swatch ends at its right.
- The swatch of the current color has a ring in the text color around it.
  The custom swatch shows a plus on the button fill while the color is a
  swatch's and, otherwise, that color with the ring. Hovering a swatch shows
  its `#RRGGBB`, and the custom swatch says it is custom, with the color
  when it is.
- A click on a swatch applies its color. A click on the custom swatch opens a
  picker below it, right-aligned: a saturation and value square, a hue bar,
  and a hex field, without alpha. A drag or a typed color shows in the custom
  swatch at once and applies on release or when the hex field's editing ends.
  Escape leaves an active hex field first, then closes the picker, then the
  window; a click outside also closes it.
- The new accent shows everywhere from the next frame, including the
  `Accent` [frame](#menu-frame). The status panel and state colors stay as
  they are.

### Key bindings

The `[Controls]` page binds each key ([Configuration](configuration.md#controls))
by pressing it, as games' control menus do.

- A row's button shows its key's label, or that it is unbound in muted
  text. A click on it, on the press, makes it listen: the button asks for a
  key with an accent tint and outline. Nothing is added under it, so the rows
  below stay in place.
- The next key pressed is bound and applied at once, and listening ends.
  Escape, the key it already has, and any mouse click end it without a
  change; a click on another key's button makes that one listen instead.
- Backspace unbinds the action and applies at once; on an unbound action it
  ends listening without a change. `OpenSettings` and `OpenChat` are the only
  ways to open their windows, so Backspace there restores the default,
  `F10` or `Enter`, instead, checked against the other actions like a
  pressed key.
  `SettingsVisitor::Key` passes the value Backspace sets, `a_cleared`: 0 for
  an action that can be unbound.
- A reserved key, one without a name, shows a red line under the button
  saying it cannot be bound, and a key another action has a line naming
  that action by its row label; listening goes on, so the player
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
rows below hold `ConnectAutomatically`, `CustomServer`, and
`ServerEndpoint`, without a group heading, so every setting that enables or
disables the button is on the same page. `SettingsWindow` draws `[Connection] DisplayName` as a label
over one line of
its text field and the connection button, as multiplayer clients lay out
joining a server, and `ConnectionControls` draws the button (`DrawButton`)
and one line under the two (`DrawNotice`). The section has no status, server
name, or ping of its own: the sidebar's summary and the status panel show
those.

- The field takes the page's width less the button, has a placeholder while
  empty, and applies like any text setting, when its editing ends. Applying
  it by a click elsewhere never connects.
- The field takes only the characters a display name may hold
  (`Multiplayer::IsDisplayNameCharacter`) and at most
  `Multiplayer::kMaximumDisplayNameLength` of them (`FilterDisplayName`):
  other typed or pasted characters are dropped, and a paste past the limit
  is cut at it. Text shorter than `kMinimumDisplayNameLength` is still
  invalid, so the field can be red while a name is being typed, with the
  minimum under it.
- Enter or keypad Enter in the field applies its text and, when the button
  could connect (`ConnectionControls::CanConnect`), connects like a press of
  the button; otherwise it only applies. The window stays open. The editor
  only records the request, and the window connects after
  `ApplyLiveSections`, so the hello never carries the display name from
  before the edit ([Network](network.md#sessions)).
- The field is editable only while `Network::GetConnectionState` is
  `kDisconnected`, because a session takes its display name from the hello
  ([Network](network.md#sessions)), and hovering it then shows the setting's
  comment. Otherwise it is disabled, and hovering it shows why instead
  (`ConnectionControls::GetDisplayNameLock`): the connection must be
  cancelled while connecting, and ended while connected or disconnecting.
- Opening the window with no display name applied puts the keyboard in the
  field. Escape then leaves the field, and a second press closes the window.

The button and Enter in the field are the only controls that connect. The
button follows
`Network::GetConnectionState`, so its label never claims a state the transport
has not reached, and keeps its width and identity when the label changes:

| State | Button | Fill | Press |
| --- | --- | --- | --- |
| Connected | disconnect | red | stops wanting a connection |
| Connecting | cancel | plain | stops wanting a connection |
| Disconnecting | disconnecting, disabled | plain | — |
| Disconnected, the field holds a display name | connect | accent | wants a connection |
| Disconnected, the field is empty or its text is invalid | connect, disabled | plain | — |
| Disconnected, `CustomServer` on without a URL | connect, disabled | plain | — |

Connecting goes by the field's text, not by the applied display name: pressing
the button ends the field's editing, which applies the text before the press
lands, so a name that was only typed connects with one click.

While disconnected with `CustomServer` on and no URL, hovering the disabled
button shows how to connect (`ConnectionControls::GetButtonLock`): enter a URL
in the field below. This is the only place the plugin shows it.

The line under the field and the button is wrapped at the page's width and
shows the first of these that holds. It is the only place the plugin shows
them. Otherwise it stays empty at one text line's height, so the rows below
do not move when a message appears:

| Condition | Line |
| --- | --- |
| The field's text is not a display name ([Network](network.md#sessions)); the field's filter leaves being shorter than `kMinimumDisplayNameLength` as the only way | how many characters a name needs at least, red |
| Disconnected because the server refused the connection, for its display name (`kDisplayNameTaken`, `kDisplayNameInvalid`) or otherwise (`kProtocolMismatch`, `kSessionReplaced`, `kServerFull`, `kTooManyNewSessions`) | the sentence of `Network::GetConnectionRefusal`, red, until the button connects again |

A divider in the separator color (`DrawConnectionDivider`) follows the line
and parts the bar from the rows below. The gap below it equals the gap from
the field's bottom to the divider while the line is empty, so the divider sits
midway between the field and the first switch.

The sidebar's summary (`ConnectionControls::DrawSummary`) is an indicator and
the state on one line, with the ping in muted text at the right while
connected:

| State | Indicator |
| --- | --- |
| Connected | green |
| Connecting | amber, pulsing |
| Disconnecting | grey |
| Disconnected, refused by the server | red |
| Disconnected | grey |

A game that starts with `ConnectAutomatically` and a display name connects
by itself;
nothing else connects without a press. A refusal leaves the player
disconnected with the field editable, and the button then retries with
whatever is applied, which clears the sentence. The refusals are in
[Network](network.md#connection).

Not wanting a connection closes it and removes every remote player until the
next connect. The button acts on the live settings, which a display name or
endpoint reaches when its editing ends. Applying an endpoint or switching
`CustomServer` keeps a live connection on its server; the next connect, or
a reconnect after a loss, goes to the server they now name
([Network](network.md#connection)).

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
- The closed field shows the item's name, its `Plugin|0xID` marked as not
  loaded in amber for a missing plugin, or the name in amber for an item the filter rejects,
  each with an explaining tooltip. A value for a plugin that is not loaded can
  still be written to the file.
- The open list is 40 × 22 font sizes, with a search field that takes keyboard
  focus and a two-column table of name and plugin. There is no empty row: a
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

`UI::ItemPreview` renders one item at a time through the game's own inventory
renderer, following `Item3DPreview` in ModExplorerMenu: as the model of one
panel (the trade window's [preview column](#preview-column) and the item
picker's panel) or as cached thumbnails for many cells.

- `DrawModel(formID, size)` takes the area as an invisible button and draws
  the item's model fitted and centered in it over whatever is behind it; the
  item picker draws its dark panel first. A new item shows nothing for
  0.3 s, then a loading line, and a no-preview line once it fails. While the button
  is held, the mouse turns the model, one radian per 8 font sizes of
  movement, the horizontal part negated as the inventory menu's mouse
  handler does. A new item starts unturned.
- Once captured, the model spins about its vertical axis at
  `kSpinRadiansPerSecond` (30° a second, one turn in 12 s; the user's
  choice), as a horizontal drag to the right turns it. Holding the button
  pauses the spin, which resumes from where the player left the model as
  soon as it is released (the user's choice).
  The spin advances by the frame time of the updates that render the model,
  so it pauses rather than jumps while thumbnails hold the renderer. A light
  (`FormType::Light`) does not spin, since ID 51778 does not turn it.
- `Update` runs in `Overlay::DrawFrame` before `ImGui::NewFrame`, when the
  game frame is finished and the overlay has not drawn. It advances a clock
  by the clamped frame time and picks one form to have loaded
  (`ChooseWork`): the model's when it is urgent (its first capture, a turn,
  or a new size), else a still waiting for its first capture (the loaded
  form's, else the earliest requested), else the model's for its refresh
  or while a fresh load is watched, else a still due for its refresh, else
  an [animated](#3d-preview) or spinning model's, else none, which stops
  the renderer (`End3D`) and keeps the images. Pending stills come before
  the model's refresh and glow watch because those held stills back for up
  to 3 s while the player moved between items (2026-10-14 log).
  `Inventory3DManager::Begin3D(kInventory)` is called once, and
  `LoadInventoryItem` only when the form changes or a refresh needs a load
  that started after the capture it replaces.
- A load serves its whole form (`ServeLoaded`): once it is ready, the same
  update captures every still of the form that a window drew last frame
  and that is due, then the model if it is the form's and due, animated,
  or spinning. So adding the hovered item to an offer captures its cell's
  still from the model's load on the next update, without loading again.
  A model loading in the background is
  attached only by `Render`, so while none is attached, `Update` calls
  `Render` with `UI::numDisablePauseMenu` at zero, which draws nothing. A
  model counts as attached only when the last `loadedModels` entry's
  `itemBase` is the loaded form, since the previous item's model was seen
  still attached a frame after a new load. A job captures once the model is
  attached, one more update pass has run, and every diffuse and normal
  texture of its lighting materials has a `rendererTexture` with a D3D
  texture and view; a model that has not rendered within
  `kCaptureTimeoutSeconds` (5) of frame time fails and logs why, and one
  whose textures are still missing then is captured as it is. The timeout
  counts only updates on which the load was not ready or a capture failed,
  and it fails every request of the form the load was serving.
- A capture projects the model's world bound through
  `UI3DSceneManager::viewFrustum` to a screen rectangle, saves that
  rectangle of the back buffer, clears it
  (`ID3D11DeviceContext1::ClearView`), calls `Render` with the count at least
  one, copies the rectangle out, and puts the saved pixels back.
- The model is rendered twice, cleared to black and to white, so its
  coverage can be told from the back buffer's alpha, which is not coverage.
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
  the `fInventoryMenuLight*` INI point light (by default at (100, -350, 100),
  radius 400, in front of and above the item), stays as the inventory menu has
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
  after the panel showed the same item got the panel's size and turn
  (2026-10-10 log: thumbnail boxes of about 1176 px right after a model
  capture of the form). The first time a form attaches after `Begin3D`,
  `ItemPreview` keeps its local transform as the game's (`gameTransforms`,
  cleared on stop). Each still capture starts from it, and each model
  capture from the model's kept transform (`SetTransform`), since both
  share one load.
- A model is animated when a node, geometry,
  or geometry property in its scene graph has a time controller, or a
  shader property has effect data (`BSShaderProperty::effectData`, which
  is where an enchantment's glow is). The check runs after each capture
  until one finds either. The game attaches the glow a frame or two after
  the model (2026-10-10 log: absent at a load's first capture, present at
  the next). A form with its own enchantment
  (`TESEnchantableForm::formEnchanting`; items load without extra data, so
  no other can glow) is therefore watched: after the first capture of each of its loads, the
  refresh's included, the model is captured on every update for
  `kEffectWatchSeconds` (0.5) while no still waits for its first capture,
  as otherwise it showed its glow only once turned. A still, captured
  once per load, instead waits for effect data until
  `kStillEffectWaitSeconds` (0.25) have passed since its load attached or
  in all across its loads (`ThumbnailSlot::effectWaited`, so a model
  taking the renderer back does not restart it), running only `Render`'s
  update pass with the count at zero meanwhile; one without a glow by then
  is captured as it is. The glow came within 6 to 9 ms of the first
  capture (2026-10-14 log), and Ruin's Edge (FE002D63), enchanted but
  animated by a time controller, never got effect data, so the earlier
  0.5 s wait from each load held back every still after it. An animated model is captured on every
  update that has no other job, as the inventory menu renders it each
  frame; captured only when due, its glow froze between captures and
  jumped at each turn, which a drag showed as flicker (user report,
  2026-10-10). Afterwards it gives way only to a thumbnail that is due, so the loaded
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

## Engine assumptions

To verify in game:

1. A model captured again 1.5 s after its first capture, from a fresh
   load, is drawn with all its textures. In game some first captures,
   taken one update pass after attaching, lacked textures even though
   every lighting texture already had its `rendererTexture`: in 1.7.104
   `BSShaderManager::GetTexture` (ID 105640) reads a texture file
   synchronously on the model loader thread, and `BSShaderResourceManager`
   (vtable slot `0xD0`, ID 108531) sets `rendererTexture` when it creates
   the `NiSourceTexture`. What the first capture misses is not identified.
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
   5 s timeout; no capture in game has timed out so far.
5. The UI 3D scene draws a model the same over black and over white within
   one frame, so the difference of the two captures is its coverage: no
   part of the scene render reads the cleared background.
6. ID 51778, called as the inventory menu's mouse handler (ID 51770) calls
   it, with a `NiPoint2` of angles in radians, turns the last loaded model
   about its bound's center by changing its local transform. From the
   disassembly, `Render` does not reset that transform: its zoom step (ID
   51777) moves the model only while a zoom is under way. Setting the kept
   transform on a newly attached model before its update pass shows it
   turned.
7. `D3DCompile` from `d3dcompiler_47.dll` compiles the overlay's shaders for
   `vs_4_0` and `ps_4_0` (they were only compiled by DXC for shader model
   6), and the state `PassScope` puts back is all the engine's state
   tracking relies on, so the frame after a blur or a model capture draws
   as before.
8. The back buffer at the overlay hook is single-sampled and can be copied
   whole (`CopyResource`) into a texture of its own format.
9. Scaling a model's local transform in the inventory scene keeps its
   lighting and textures as the unscaled model has them, and calling
   `NiAVObject::Update` (ID 70251) with a time of zero on the attached model
   advances nothing visible. From the disassembly, a controller accumulates
   the time since its last update (`NiTimeController::ComputeScaledTime`,
   ID 70824), so the zero steps it back by the last time and the update
   `Render` makes before drawing, at the application's run time (ID 410201,
   in ms), steps it forward again.
10. Clearing `taaEnabled` for the scene render's call removes the jitter,
    the motion vector target, and the TAA resolve together, read from the
    disassembly (the camera setup it calls, ID 77506, takes the same flag),
    and nothing else that frame reads the flag between the clear and the
    restore.
11. An enchanted item's glow is attached by the game within
    `kEffectWatchSeconds` of the model's attach. The 2026-10-10 log showed
    it as effect data on one shader property of enchanted armor (no
    controllers), absent at the first capture and present one update
    later, its `vOffset` advancing between frames; what attaches it, and
    whether it can take longer, was not traced. Only forms with their own
    enchantment get one, and `Render` with the count at zero, which draws
    nothing, still lets it attach.
12. The lighting shader reads the directional ambient and ambient specular
    from `BSShaderManager::State` during the scene render's call, so setting
    them with ID 105643 for the call and restoring the fields afterwards
    lights the capture without changing the world's frame. Read from the
    disassembly: ID 105643 writes only those fields and its color copy at
    ID 414646, the inventory scene's render path does not call it, and the
    lighting shader's geometry setup (ID 107312) reads the transform. How
    bright 0.45 looks against the scene's light was not seen in game.

13. Only an English and Russian switch was seen on Windows; other
    languages were not tried. Dead keys compose in the polling thread's
    own keyboard state, so a dead key pressed while the poll changes
    threads may be lost.
14. Nameplates: `Actor::HasLineOfSight` (ID 53829) on the player calls ID
    40520, which sets its second argument from a frustum test of the
    target's bound and only then casts a ray, on collision layer `0x29`,
    from the camera; this was read from the 1.7.104 disassembly, not seen
    in game. That other remote actors and the target's own collision do not
    block the ray is assumed. `WorldRootCamera`'s `worldToCam` at the
    overlay hook is the matrix the frame was rendered with, its fourth row
    gives a depth that is positive in front of the camera, and its viewport
    is 0 to 1 with y up; ID 70640's arithmetic matches a standard
    projection, but none of this was seen in game. The SE IDs (53029,
    69270, and 39444 for ID 40520, with the same layer) match the 1.5.97
    disassembly.

## Acceptance criteria

1. The main menu shows no status panel, and a game shows none by default. With
   `StatusPanel = true`, in a game the panel shows the connection, a stable
   ping, traffic, and plugin time, and scales at 1080p and 4K;
   `StatusPanel = false` hides it at once.
2. Enter opens the chat only with no menu open; the character stands still while
   typing, and W held from before keeps running until released. Enter sends and
   the line appears for both players, and closes the chat, or with
   `CloseChatOnSend = false` keeps it open for the next line, where Enter in
   an empty field does nothing; Escape closes without sending and does not
   open the journal; a menu that opens by itself closes the chat; recent lines
   fade after 10 s; the wheel and Page Up/Down scroll; sending while disconnected
   shows a failure note above the field and keeps the chat open with the
   text, and typing clears the note.
3. The chip shows Nearby at first, and `hi` reaches players within 8192 units
   or in the same interior with a lime name, and not a player farther away.
   Tab turns the chip to Global; the line reaches every player with a gold
   name, and so does the next one, until Tab again. `/g hi` is sent as
   written.
4. `F10` opens the settings window with a cursor, and the game receives no input
   until it closes; `F10` again closes it. A new `OpenSettings` key opens and
   closes it and the old one no longer does, as soon as it is bound.
   Chat still opens with Enter and shows no cursor until the mouse moves.
   With Modex installed and bound to Delete, Delete and letters typed into the
   chat or a settings field do not open its menu, and Delete opens it once
   the window is closed; `PlayerHub.log` has one line about detouring input
   dispatch again.
5. The `[Connection]` page holds the display name field with one button beside
   it and a line under them. Opening the window with no display name puts the
   cursor in the field, the button cannot connect, and the status panel shows
   only that it is disconnected; typing a name and then clicking the button
   once or pressing Enter connects, and the window stays open, while a click
   elsewhere only saves the name. Enter with an invalid name, or with
   `CustomServer` on and no URL, only leaves the field. Spaces, punctuation,
   and non-ASCII letters cannot be typed or pasted into the field, and it
   takes no 16th character. Once
   connected the button is red and disconnects, the field is greyed out and
   says on hover to disconnect first, and the sidebar shows the connected
   state with a green indicator and the ping; disconnecting makes the button
   orange and connecting with the field editable and the sidebar
   disconnected with a grey indicator, removes remote players, and keeps
   them away through a save load until the next connect. Connecting right
   after disconnecting shows a plain cancel button and a pulsing amber
   indicator until the connection opens, never a disconnect button while
   disconnected. A display name another player holds shows a red indicator
   and the refusal's sentence under the field beside the button, and only
   the disconnected state in the status panel; entering another display name
   and connecting clears it. A name shorter than three characters turns red
   with the minimum of three under the field and disables connecting. The sidebar shows the
   same status in every section.
6. `[Connection]` has nothing below its settings, and `[UI]` and
   `[Advanced]` only a disabled Defaults button. Switching `StatusPanel` off hides the panel at once; typing
   a display name changes nothing until a click elsewhere, which saves it
   without connecting. `[Connection]` shows `ConnectAutomatically`
   on, `CustomServer` off, and no URL, without a group heading.
   `ViewRadius` is a slider showing its zero name at the left end and game
   units up to 15000 in steps of 100 as it is dragged, saved on release. After changes on
   `[UI]` and `[Advanced]`, each page's Defaults restores only its own
   settings, leaves the chat's size as dragged, and is disabled again. Each
   change is in `PlayerHub.ini` without pressing anything. `[DefaultOutfit]`
   shows Defaults, Revert, and Apply; a picked item shows a reset icon and reaches
   peers only on Apply, and Defaults leaves the display name and endpoint as
   they are.
7. The picker lists only matching base-game items and races. Hovering rows
   switches the model without flicker on the world behind, and the model spins
   slowly; background loads
   appear after the attach pass (a stall logs `Item preview waiting`); nothing is
   left on screen after the list closes; the inventory shows its own models
   normally afterwards; FPS stays steady while a list is open;
   `ItemPreview = false` removes the panel.

8. With the default accent the settings window has no blue: the open
   section's sidebar item, switches that are on, the connection button and a
   pending Apply, and hovered or pressed widgets are orange at 1080p and 4K, and the
   lists it opens have a rounded `FF9000` border. Picking a sidebar item shows that section, and the window can be
   dragged by its sidebar or title. The status panel looks as before.
9. The settings window, its lists, and its tooltips are in Sovngarde, with
   section titles and group names bold, at 1080p and 4K. The
   open chat, with its filters, field, name list, and menus, and the recent lines
   are in Sovngarde too, with each line's label, up to its colon, bold,
   and the lines stay in the same column and at the same
   height when the chat opens; the status panel stays in the system font. The
   log has one `UI overlay: font` line per file. A display name or chat line
   with characters Sovngarde lacks still shows every character. With
   `PlayerHub/fonts/` removed, the log warns for each file and the window and
   the chat show in the system font.
   The sidebar is headed by the sharp PlayerHUB logo at 1080p and 4K, left-aligned
   with the section labels and as wide as the space between their insets, its bar
   in the accent color and following a new accent from the next frame; without
   `PlayerHub Logo.ttf` the log warns and the sidebar shows a text title.

10. The settings window and the open chat have the game's menu frame in the
    game's grey, with a knot in each corner, at 1080p and 4K; nothing in
    either window touches a knot, and resizing the settings window keeps the
    knots in its corners. The `BorderThickness` slider shows 70%; dragging it
    to its right end, 100, makes the frame as large as on a message box of the
    game once released, its left end is 50, and at each the content stays
    inside the lines and clear of the knots.
    Picking
    `Accent` for `BorderColor` on the `[UI]` page turns both frames
    the accent at once and writes `BorderColor = Accent` to `PlayerHub.ini`;
    `Vanilla` turns them grey again. Recent chat lines have no frame and do
    not move sideways when the chat opens. The log has one
    `UI overlay: menu frame` line. With `PlayerHub/frame/` removed, the log
    warns and both windows have a rounded line in the frame color.
11. With `Ulf Stone` online, `@u` lists it; Tab or Enter turns the chip to
    a whisper to that player and empties the field, and a line reaches only
    that player, labeled in violet as sent to it for the sender and as from
    the sender for the recipient. Enter, text, Enter whispers the same player again.
    Backspace in the empty field leaves the chip as it is. A name nobody has shows one row
    saying so. Once the player disconnects, a note above the field says it
    is offline and the chat stays open with the text.
12. Up in the field recalls the last sent line with its target, and Down
    returns the text that was being written. Closing and opening the chat
    clears a note.
13. Moving the mouse in the open chat shows a cursor in the middle of the
    chat, and the character does not turn or attack. A click on another
    player's name turns the chip to that player; a right click on its line
    offers whisper, mute, and vanish; a click on the chip lists
    Global, Nearby, and the latest partners. Typing continues after each click,
    and a click outside the chat changes nothing.
14. The Global, Nearby, and Whisper filters are all on at first. A
    click on Global turns its chip grey and leaves the other two and the
    send target as they were; a global line then shows neither in the open
    chat nor among the closed chat's lines, and Global gets a dot on its
    corner. With Whisper off a
    whisper stays out of the closed chat too. A second click shows the
    channel again with the lines that arrived meanwhile. With Global off,
    Tab to Global turns it on, and so does the player's own global line coming
    back. With all three off no line shows.
15. With the default accent the open chat has no blue and no square corners,
    at 1080p and 4K: the
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
    `PlayerHub.ini` has `ChatWidth` and `ChatLines`, the closed chat's lines
    are as wide, the chat has that size after the game restarts, and the
    `[UI]` page shows neither key.
17. The open chat's filter row ends with a chip showing a person and the
    number of players online, or that it is offline while disconnected. A click opens
    a framed list right of the chat and as high, with a search field on the
    filters' row and the other players by name, the ones nearby first and
    in the Nearby color. A click on a name turns the chip to a whisper to
    that player and typing continues in the field; a right click offers
    whisper. Typing `ul` in the search leaves the names containing it,
    and Escape there closes the chat. After Escape or Enter
    closes the chat, the list shows again when it opens, until the button
    closes it. The wheel over the list
    scrolls it and not the lines, and with the chat dragged to the screen's
    right edge the list is still on the screen. With hundreds of players
    online the frame rate with the list open is the same as with it closed.
18. A right click on another player's name in the list or on its chat line
    offers mute and vanish; on the local player's line, neither. After
    a mute no notice shows, the player's lines vanish from the open and
    closed chat, earlier ones included, and its new lines and whispers do
    not show, and its whispers do not move it in the Tab order. After a vanish
    its character disappears at once while the other player still sees this
    one and is told nothing, and it stays gone after either game reconnects
    or restarts. The list marks the player with a struck bubble or eye.
    Unmuting shows every held line again, and unvanishing brings the
    character back where the player stands, if it is in range.
19. The button at the bottom of the player list opens a framed window in
    the middle of the screen over the open chat, with a tab each for
    friends, muted, and vanished players. Each lists the connected server's
    players, offline ones included, those connected first with a green dot
    and the others after with a gray one, which tell online from offline
    under the cursor, and a search field while the tab has anyone; a row's
    button removes the row at once and has the effect the menu has. A name is
    underlined under the cursor; a click whispers an online player, and a
    right click opens the player's menu, an offline player's with
    whisper disabled. Escape closes the window
    and leaves the chat open; closing the chat closes it too. Connecting to
    another server lists only that server's players.
20. Adding another player as a friend from its menu, in the chat, the
    player list, or the world, adds it to the friends tab with no notice,
    and the menu then offers to remove it; the other player is told nothing.
    When that player next connects, a notice on the right says so, with the
    chat open or closed, and the chat's lines do not; friends already online when this game connects are not
    announced. An offline friend shows its last name in the friends tab.
    A friend stays one after either game restarts, and muting or vanishing
    a friend keeps the friend.
21. Pressing E on another player's character, or the horse it rides,
    shows a cursor and a menu at the crosshair with the player's name, race,
    and level, then whisper, mute, and vanish, and no dialogue starts. Mute
    and vanish act as in the chat's menu. Whisper opens the chat with the
    chip whispering
    that player. Escape, E again, or a click outside closes the menu without
    opening the journal or activating anything, and the character moves
    again at once.
22. On the `[UI]` page, a click on the blue swatch turns the settings window's
    sidebar mark, switches, slider, and hovered widgets blue at once, and
    with `BorderColor` at `Accent` its frame and the open chat's too; the
    open chat's player list button and lists follow. `PlayerHub.ini` then
    has `AccentColor = 4F8EF7`, which the next game loads. The custom swatch
    opens a picker; dragging in it changes only the custom swatch until
    release, then the whole window, and a typed hex color applies on Enter.
    A dark custom color gives the connection button light text. Amber returns the
    default look. The status panel stays as it was.
23. The menus a right click opens on a player's chat line or on its name in
    the player list look like the one E opens: its name in bold in the
    accent color, then a muted line with its race and level, also for a
    player far away, whose line fills in a moment later without the menu
    changing height. A player using a race the local game lacks shows the
    race its character has locally. With the accent changed, the name
    follows it.
24. A new install connects to the official server, and its URL appears
    nowhere in the settings window or `PlayerHub.ini`. Switching
    `CustomServer` on while connected keeps the connection and remote
    players, and a URL field without a label appears under it. Disconnecting
    with the field empty disables connecting on `[Connection]` with a sentence
    saying why. A URL typed into the field applies on Enter, and connecting
    joins that server; an endpoint without `http://` turns red. Switching
    `CustomServer` off while connected to the custom server keeps that
    connection; disconnecting and connecting join the official server.

25. Activating a nearby player and picking trade shows both players the
    prompt at the top, which `Y` and `N` answer for the other player,
    without moving the character or opening a game menu. Accepting opens
    the trade window on both screens with the cursor, also over an open
    chat. A click on a stack asks how many, starting at 1, and only the
    popup's button or Enter offers them; they show in the own grid as its
    thumbnail with its count. Clicking the stack's row again starts at that
    count and updates it. Clicking the cell opens its count and a remove
    button, and
    stepping the count down from 1 removes it; clicking a cell that offers
    one item removes it with no popup, and right clicks do
    nothing. The other window shows it
    within a moment, shaded until the cursor passes over it, and keeps Accept
    disabled until every line of the offer was hovered; a count change
    shades only that line again. An accepted side turns green. A modded item the other game lacks
    never appears in either offer, and its giver gets a notice naming the
    plugin and sees the item's row disabled. Escape, Cancel, and a
    message box or dialogue a script opens each close both windows.
26. Both trade grids fill with thumbnails of the offered items within a
    second or two, each on a dark or, for an enchanted item, blue cell,
    every one textured within about 2 s (`latest.log` shows `pending=0`
    on each `Item thumbnail captured` line), and keep them while the
    window is open; FPS stays steady meanwhile. With 15 lines offered
    every cell is filled, the grid does not scroll, the full mark shows
    right after the own grid's heading label and goes when a line is
    removed, rows not yet offered are grayed out, clicking one does nothing
    and posts no notice, and hovering one still shows it in the preview
    column, while an offered row's count can still change. With `ItemPreview = false` the cells show names
    and the preview column only the card. The inventory looks normal
    afterwards. An item's thumbnail and preview model look equally bright
    in a dark interior, outside by day, and outside at night, with no face
    left black, and the world's lighting is unchanged while they capture.
27. Notices stack on the right edge from 40% of the screen's height, at
    1080p and 4K, each with a strip in the accent: a completed trade, the
    other player's decline, a friend connecting. Each fades after about 5 s, a long one later; the same
    notice twice shows twice; a fifth pushes out the oldest. They show over the inventory and under the open chat or trade
    window, and never add a chat line.
28. The open trade takes the screen at 1080p, 1440p, 4K, and 21:9: the
    game behind it, HUD included, is blurred and darker and keeps moving,
    the panel takes the same share of the screen's height on each, and the
    preview column sits to its right; nothing else on screen changes, and
    the game looks normal after the window closes. FPS stays steady with
    the window open. Hovering a row or a cell shows its model at once,
    textured, floating over the blur without a box or a dark fringe, as
    the inventory menu shows it, and its card with its numbers laid out
    as the inventory menu's, in capitals where the game shows them; moving off
    the panel keeps it. The model spins slowly and evenly, one turn in
    about 12 s, without flicker. Dragging the model stops the spin and turns
    it smoothly like the inventory menu's model; on release it spins on
    from there, and it keeps its angle after other items were hovered
    and the grid thumbnails captured. The item picker's model shows on its
    dark panel as before and spins too.
29. The settings window's sidebar lists `[Connection]`, `[DefaultOutfit]`,
    `[UI]`, `[Controls]`, and `[Advanced]`. `[UI]` shows its settings under
    the Display, Players, Chat, and Items headings, and `[Advanced]` under
    Performance and Diagnostics, `StatusPanel` alone in the last.
    `[Controls]` lists `OpenSettings`, `OpenChat`, `ManagePlayers`,
    `AcceptTrade`, and `DeclineTrade` with their keys, and `[UI]` lists none. A click on
    `AcceptTrade`'s key, then `H`, shows `H` and saves `AcceptTrade = H`
    under `[Controls]`; a trade invitation then shows `H` as the accept key
    and `H` accepts it. While a button listens, `F10` shows that
    `OpenSettings` has it and does not close the window, Escape cancels
    without closing it, the game sees none of these presses, and a click
    anywhere cancels. Binding `OpenSettings` to Num 7 makes Num 7 open and
    close the window and F10 do nothing. A display name typed with a letter
    bound to `OpenSettings` keeps the letter and the window open. Backspace
    on `AcceptTrade`'s button shows it unbound, saves `AcceptTrade =`, and
    the next invitation shows only the decline hint; `Y` then reaches the
    game. Backspace on `OpenSettings`' button restores `F10`. The footer's
    Defaults button is disabled with the default keys; after rebinding any,
    a click restores `F10`, `Enter`, `O`, `Y`, and `N`, saves them, and is
    disabled again.
30. With `OpenChat` bound to `T`, `T` opens the chat with an empty field and
    Enter and keypad Enter reach the game; Enter still sends and closes it.
    Backspace on `OpenChat`'s button restores `Enter`. `O` opens the player
    manager alone in the middle of the screen with the cursor shown and the
    chat closed, its recent lines still fading; `O` again changes nothing,
    and Escape or its close button returns input to the game. Clicking an
    online friend's name there closes it and opens the chat whispering
    that friend. With `ManagePlayers` unbound, `O` reaches the game.
31. On Windows with English and Ukrainian layouts, switching the input
    language with the Windows hotkey while the chat's field is open makes
    the next letters Cyrillic, and switching back makes them Latin, without
    closing the chat. The log shows the new layout with the first
    character typed in it.
32. Another player standing in view within 30 m has their display name
    above their head, following it while they walk, run, sit, ride, or
    crouch, in third and first person, at 1080p and 4K. A friend's name is
    in the accent and changes at once when the friend is added or removed.
    Walking behind a wall or a hill fades the name out within about half a
    second and stepping back into view fades it in. Beyond
    `NameplateDistance` the name is gone, fading over the last stretch.
    Turning the camera toward a player shows the name almost at once. No
    name shows in the main menu, the pause or inventory menus, dialogue,
    with the HUD hidden (`tm`), behind the trade window, or with
    `Nameplates` off. Many players in view do not lower the frame rate
    noticeably.
33. The `FontScale` slider at the top of the `[UI]` page shows 100% and moves
    in steps of 5 between 80 and 130. Releasing it at 130 makes the text and
    every overlay window, the chat, notices, and nameplates larger from the
    next frame and writes `FontScale = 130`; the open settings window grows
    around its center and stays on the display, and the cursor moves at the
    same speed. At 80 all of them shrink the same way. `FontScale = 107` or
    `150` in the file warns in the log and loads `100`.
36. The settings window first opens centered at 48 × 35 font sizes
    (768 × 560 px at a 16 px font). Dragged to a corner, resized, closed, and
    opened again, during gameplay or after another menu, it is where and as
    large as it was left; after a game restart it is centered again at the
    default size.
34. In a game with an empty `DisplayName`, the framed hint at the top center
    names the `OpenSettings` key and the `[Connection]` page; rebinding
    `OpenSettings` changes the key it names. It is gone in the pause,
    inventory, and map menus, in dialogue, with the HUD hidden (`tm`), and
    while the chat or settings window is open. Pressing the key opens
    `[Connection]` with the keyboard in the field, and applying a name there
    leaves no hint after the window closes. Clearing the name brings it back.
35. The `[UI]` page's `ChatPositionHorizontal` and `ChatPositionVertical`
    sliders show 1% and 8%, with the chat above the magicka bar as before; at `50` and `40`
    the open chat and the recent lines sit around the
    screen's middle, in the same column, at 1080p and 4K alike; a chat
    dragged wider there grows to the right up to the screen's edge and its
    left edge stays put, and one too large for the room at `70` is narrowed
    and shortened to fit while `PlayerHub.ini` keeps its size. At `100` and
    `100` the chat is at its minimum size against the upper right corner and
    wholly on the screen, and the player list opens on its left; at `0` and `0` it is
    against the lower left corner, over the magicka bar. Defaults puts it back
    above the magicka bar, and the positions are in `PlayerHub.ini` after a
    restart.
37. Dragging the open chat by its left or bottom strip, a gap in the
    filters' row, or the lines away from a name moves it with the cursor and
    the move cursor, keeping its size; it stops at each screen edge, wholly on
    the screen. A press on a filter, the players chip, the target chip, the
    field, the scrollbar, or a name does what it did before and moves nothing,
    as does a click without a drag. On release the chat stays within a few
    pixels of where it was dropped, at its size, the `[UI]` page's sliders
    show the new percentages, and the chat is there after a restart. Dragged
    into the upper right corner with the player list open, the list opens on
    the chat's left.

## Known limitations

- A player's menu shows the race and level of a player without a character
  in range a server round trip after it opens. Opening more than five menus
  within a few seconds can exceed the server's
  [inspection limit](network.md#inspection), and those menus show the name
  alone.
- Any accent is accepted, so a very dark one or one close to the surfaces
  makes selection and hover marks hard to see; picking amber restores the
  default. Live sections have no reset button
  ([Settings window](#settings-window)).
- The accent follows a picker drag only on release, since each apply saves
  the file; the custom swatch shows the color meanwhile.

- A window over the right edge, such as a wide trade window, hides the
  notices under it; they keep their time meanwhile.
- Nameplates are tested from the camera to the actor, so a name stays shown
  while only part of the character is in sight, and keeps its result for up
  to 0.2 s after the view changes. Plates of players standing close
  together overlap; nearer ones are drawn over farther ones. A plate shows
  while another plugin's menu that neither pauses nor hides the HUD is
  open.
- Lists, tooltips, notices, notes, and recent chat lines have no menu frame: its corners need
  about 65 px of height at 1080p at the default thickness. The settings window's
  resize grip and edge highlight and the chat's grips are not drawn, so only
  the cursor shows where each resizes. Nothing shows where the chat can be
  grabbed to move it, and the move cursor shows only once it moves.
- `BorderThickness` sizes the whole frame; thinner lines with knots of the same
  size would need another shape. Below about 60 the knots are hard to read at
  1080p.
- The frame is exported ahead of time from the vanilla `messagebox.swf`, not
  read from the player's game, so a mod that replaces the game's menu art does
  not change it.
- Keys are keyboard keys only: mouse buttons and the gamepad cannot be bound,
  and neither can a key with a modifier. Labels are a US layout's, since
  scan codes are positions: on another layout a label can differ from what
  the key prints.
- A key a mod or the game also uses is bound without a warning; only the
  plugin's own keys are checked against each other.
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
- An input method editor's composition, such as Chinese or Japanese, does
  not type into a window: Skyrim takes only single `ToUnicode` characters.
- Editing shortcut letters (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY positions,
  not the keyboard layout. Chat lines and player names cannot be selected
  or copied.
- `@Ulf Stone cold` typed in full whispers `Ulf Stone` while that player is
  online, since the longest name wins; whispering `Ulf` then takes picking
  the name from the list.
- The chat is resized with the mouse only, and moved only from the settings
  window. A word wider
  than the chat is cut at the row's end wherever that falls, not at a
  syllable or after punctuation.
- The player list sorts by folded bytes, so `Aela 194` comes before
  `Aela 2`, cuts a long name without an ellipsis, and is picked from with
  the mouse only. It shows names and who is nearby, not levels or places,
  which the roster does not carry.
- The chat's filters are toggled with the mouse only. Names in scripts other
  than Latin, Greek, and Cyrillic match only in the case they are written in.
- A click made before the mouse has moved since the chat opened is dropped.
- The send target, the filters, the whisper partners, and the sent lines last
  until the game exits.
- An offline player in the player manager shows the name the roster last gave
  it while this game ran, or the kept one; online, the roster's. The window lists the last
  server that welcomed the client, or the configured endpoint's before the
  first connection of a game, so an endpoint changed while disconnected
  shows its list only once connected. It opens only from the open chat.
- Friends, mutes, and vanishes are kept per server URL, so the same server
  reached under another address starts with none.
- A gamepad's Activate opens the player menu as E does, but the menu takes
  the mouse and keyboard only, while gamepad input keeps reaching the game.
- The picker covers armor, weapons, ammunition, and races, not torches, spells,
  or actors; `ItemKind` is where more kinds would go.
- A trade thumbnail or model shows the base form's model, without an
  enchantment's glow or the item's own name. Each is captured twice, so an
  item whose textures were missing at the first capture shows untextured
  for about 1.5 s; one still missing them at the refresh stays so until it
  is shown again. Thumbnails are captured one per frame or more, so a full
  grid fills over a moment, and while the hovered item's model loads they
  wait: sweeping the cursor across rows delays them. The atlas holds 64;
  more distinct items in view than that show names.
- The blur's strength and darkening are fixed. A display narrower than
  about 1.07 times its height leaves no room for the preview column, and
  the trade then shows no item numbers at all. The model cannot be zoomed,
  and a turn applies the drag's angles in one call per frame, so a fast
  drag turns in larger steps than the inventory menu's. The spin captures
  the model on every frame it is shown, two renders and a separation pass,
  and stops while thumbnails are captured.
