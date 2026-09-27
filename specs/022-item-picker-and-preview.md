# Item Picker and 3D Preview

## Status

Implemented, pending a Windows build and in-game check. On Linux,
`ItemCatalog.cpp`, `ItemPicker.cpp`, `SettingsWindow.cpp`, `SettingsSchema.cpp`,
and `PluginConfig.cpp` were compiled against Dear ImGui 1.92.9 and SimpleIni with
stubbed SKSE headers, and run headless with a fake catalog and a stand-in
`ItemPreview`. The checks covered: filters per key (the head slot group, armor type,
weapon type, arrows and bolts), multi-word search, case-insensitive lookup, a
list that shows only matching items, the preview following the hovered row, Enter
picking the first match, None clearing a value, Escape closing the list before the
window, Apply saving `Plugin|0xID`, and `ItemPreview = false` hiding the preview.
`ItemForms.cpp`, `ItemPreview.cpp`, and the changed `EquipmentFallback.cpp` need
CommonLibSSE-NG and were only reviewed.

## Goal

Choosing an equipment fallback no longer means typing `Plugin|FormID`. The
settings window shows each form setting as a dropdown of the loaded items that
fit it, searchable by name or plugin, with the hovered item's 3D model beside the
list. The pieces are reusable, so later windows (appearance or outfit editors,
for example) can pick items the same way.

## Non-goals

- Spells, actors, and races. The catalog holds armor, weapons, torches, and
  ammunition; `ItemKind` is where more kinds would be added.
- Rotating or zooming the model.
- Typing a form by hand in the window. A value for a plugin that is not loaded can
  still be written to `skyrim-mp.ini`; the picker shows it as not loaded.

## Source layout

```text
src/forms/
  ItemFilter.h              # ItemTraits, ItemFilter (plain data, no engine types)
  ItemCatalog.h/.cpp        # immutable, searchable list of named items
  ItemForms.h/.cpp          # engine side: GetItemTraits, GetArmorType, BuildItemCatalog
src/ui/
  widgets/ItemPicker.h/.cpp # the dropdown widget
  preview/ItemPreview.h/.cpp  # Inventory3DManager capture into an ImGui texture
```

## Item filter

`Forms::ItemTraits` is what decides whether an item can stand in for another: its
kind, and its slot mask and armor type, weapon type, or bolt flag.
`Forms::GetItemTraits` reads them from a form. `Forms::ItemFilter::Matches` checks
them against what a setting accepts.

`Config::SettingsVisitor::Form` now takes the setting's filter.
`GetArmorFallbackFilter` and `GetWeaponFallbackFilter` build the filters for
`[EquipmentFallback]` keys, and `Forms::kTorchFilter`, `kArrowFilter`, and
`kBoltFilter` cover the rest. The settings window lists only matching items, and
`EquipmentFallback` resolves configured items with the same filters (spec 019),
so the list and the live behavior never disagree.

| Key | Accepts |
| --- | --- |
| `<Slot>.Light`, `.Heavy`, `.Clothing` | armor of that armor type covering the slot |
| `<Slot>` | armor of any armor type covering the slot |
| `Head`, `Hair`, `LongHair`, `Circlet`, and their types | armor covering any of these four slots |
| weapon keys | a weapon of exactly that weapon type |
| `Torch` | a carriable light |
| `Arrow`, `Bolt` | ammunition that is not, or is, a bolt |

Head slots form one group because helmets and hoods use different ones: vanilla
helmets cover Hair and Circlet, yet they are the default `Head` fallbacks.

## Catalog

- `Forms::BuildItemCatalog` runs at `kDataLoaded`, before `EquipmentFallback`
  resolves anything. It collects every `TESObjectARMO`, `TESObjectWEAP`,
  `TESObjectLIGH`, and `TESAmmo` with a name, traits, and a plugin-local reference.
  Enchanted variants (`templateArmor`, `templateWeapon`) are left out, since they
  look like their template, which is listed.
- The catalog is sorted by name. Plugin names compare without case, as the game's
  lookup does. It never changes after it is built, and it is published through an
  `std::atomic<std::shared_ptr>`, so the overlay reads it without touching the
  engine.
- `Search` keeps the items that match a filter and contain every word of the query
  in their lowercase name or plugin. It runs when a list opens and on each change
  to the query, never per frame.
- `IsPluginLoaded` tells a value from a missing plugin apart from an item the
  catalog does not list, such as an unnamed one.

## Item picker

`UI::ItemPicker::Draw(id, value, filter)` draws one field. A window keeps one
picker for all its fields: the picker caches each field's label until the value
changes, and only one list can be open.

- The closed field shows the item's name. It shows `None` dimmed for an empty
  value, `Plugin|0xID (not loaded)` in amber for a missing plugin, and the name in
  amber for an item the filter rejects; a tooltip explains each case.
- The open list is 40 × 22 font sizes. It has a search field, which takes keyboard
  focus, and a two-column table of name and plugin. `None` is the first row. Only
  visible rows are drawn (`ImGuiListClipper`), and the current value is scrolled
  into view.
- Clicking a row or `None` picks it. Enter picks the first match. Escape closes the
  list; the settings window already leaves Escape to an open popup.
- With `[UI] ItemPreview`, a 13-font-size square panel to the right shows the
  hovered item's model, or the current value's when no row is hovered, with its
  name and `Plugin|0xID` below.

## 3D preview

`UI::ItemPreview` shows one item at a time through the game's own inventory
renderer. The approach follows `Item3DPreview` in ModExplorerMenu.

- `Draw(formID, size)` records the request and draws the latest capture, fitted
  and centered on a dark background. While a new item loads it shows nothing for
  0.3 seconds, then "Loading...".
- `Update` runs in `Overlay::DrawFrame` before `ImGui::NewFrame`, when the game
  frame is finished and the overlay has not drawn yet. If the previous frame drew
  a preview, it calls `Inventory3DManager::Begin3D(kInventory)` once and
  `LoadInventoryItem` whenever the form changes. It then projects the model's
  world bound through `UI3DSceneManager::viewFrustum` to a screen rectangle and:
  1. saves that rectangle of the back buffer,
  2. clears it to the preview background (`ID3D11DeviceContext1::ClearView`),
  3. calls `Inventory3DManager::Render` with `UI::numDisablePauseMenu` raised by
     one for the call (see below),
  4. copies the rectangle into a 2048² texture that ImGui samples,
  5. puts the saved pixels back.
- The capture is opaque, but the back buffer's alpha under the model is not
  coverage, so the image is drawn with blending turned off (an ImGui draw
  callback sets an opaque blend state, then `ImDrawCallback_ResetRenderState`).
- When no window drew a preview in the previous frame, `Update` unloads the item,
  calls `End3D`, and frees the textures. If an item menu opened meanwhile
  (`UI::IsItemMenuOpen`), that menu now owns the renderer, so `Update` drops its
  state without calling it.
- Safety checks: the preview turns itself off for the session, with one warning,
  when the overlay does not run on the bound main-loop thread
  (`GameThread::IsAuthoritative`), when the inventory renderer is missing, or when
  capture textures cannot be created for the back buffer format. `[UI]
  ItemPreview = false` turns it off by hand.
- Unlike ModExplorerMenu, the game is not paused. In 1.7.104, `Render` attaches a
  finished load (load task state 5), updates the model, and tail-calls the UI 3D
  scene render (ID 52727). That render returns before drawing anything unless the
  scene's light scheme matches and `UI::numDisablePauseMenu` (`UI + 0x168`) is
  positive. The count is positive while an inventory menu is open, and while
  ModExplorerMenu's menu is, because it sets `kDisablePauseMenu` only when it
  pauses. That is why ModExplorerMenu needs pausing. The overlay is not a menu,
  and opening one would take input focus, so `ItemPreview` raises the count
  around the call and restores it. Everything runs on the main-loop thread. The
  scene draws into the engine's current render target, which was the back buffer
  in the first in-game capture check.

## Logging

- `Item catalog built: items=N plugins=N` once at data load.
- `Item preview started` and `stopped` at debug level, and the first capture of
  each start with its screen box and bound radius, to check the projection.
  With that first capture, `Item preview capture check` reads the pixels back
  once. It gives how many differ from the cleared background, their high (alpha)
  byte, and whether the back buffer was the bound render target before and after
  `Inventory3DManager::Render`.
- `Item preview waiting` once per item whose model has not appeared a second
  after loading started. It gives the models `Inventory3DManager` holds, its load
  task state, and the manager's and scene's light schemes.
- `Item preview unavailable: <reason>` once.
- `Equipment fallback ignored ... reason=does-not-fit` for a configured item its
  key's filter rejects.

## Verification

- Linux (done): the headless checks in Status.
- In game (pending):
  - The log reports the catalog size.
  - The Equipment Fallback tab shows item names.
  - A list opens with the current item highlighted and its model shown, and
    hovering rows switches the model with no flicker on the world behind.
  - Nothing is left on screen after the list closes.
  - Opening the inventory afterwards shows its own 3D models normally.
  - FPS stays steady while a list is open.
  - `ItemPreview = false` removes the panel.
