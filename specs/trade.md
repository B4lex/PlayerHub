# Trade

## Scope

Two nearby players exchange inventory items. One offers a trade from the
other's [player menu](ui-overlay.md#player-menu); the other accepts from a
prompt; both put items into the exchange and accept. When both accepted the
same offers, each game removes what its player gave and adds what its player
received.

This is the only feature through which a peer changes the local world: only
the local player's inventory, only by an exchange the local player accepted.
The server relays and orders the exchange and checks range; each client
resolves the peer's items against its own plugins, so players with different
modpacks trade what both have.

Goal: an exchange of any items both games know, kept whole: what one player
gives the other receives, with its enchantment, tempering, name, and soul, and
nothing the receiver lacks leaves the giver.

Non-goals:

- Server authority over inventories, anti-cheat, and protection from a
  modified client or a reloaded save. A trade cannot be undone and the game
  does not save after one, so a player who quits without saving loses what it
  received while the other side stands.
- Player-made potions and poisons, poisons applied to a weapon, and any other
  dynamic form except a player enchantment, which is rebuilt from its effects.
- Trading with a player who is not near, a replay, or an in-server bot. The
  [trade bot](#trade-bot) is a real client and can.
- Ownership: the giver's stolen mark is not carried; a received item is never
  stolen.
- Showing the trade on the characters (facing each other, an idle).

Wire messages and validation are in [Network](network.md#trade); the prompt,
window, and menu entry in [UI overlay](ui-overlay.md#trade-window); the stage's
place in the pass in [Sync pipeline](sync-pipeline.md#frame-sequence); the keys
in [Configuration](configuration.md#controls).

## Components

| Module | Responsibility |
| --- | --- |
| `protocol/TradeState.h` | `TradeItem`, `TradeSkip`, close reasons, trade events, validators and size limits, `EstimateTradeItemBytes` |
| `server/internal/hub/trade.go` | A trade's players, phase, offer revisions, item counts, reports, acceptances; range, busy, vanish, expiry, commit |
| `forms/ItemInstance` | An inventory stack to a `TradeItem` or why it cannot be offered; a `TradeItem` to local forms or a skip reason; rebuilding a player enchantment; extra data on a reference |
| `forms/ItemDetails` | A resolved item's [details](#item-details) for the item card |
| `sync/trade/TradeSession` | The state machine of one trade, without engine types: phases, own offer and revisions sent, the peer's offer, reports, plugin queries and replies, acceptances, what a commit applies |
| `sync/trade/TradeInventory` | The offerable inventory, counts held, removing and adding items |
| `sync/trade/TradeSync` | The game-thread stage: events and overlay commands, cancelling on combat or a hit, batched offers, the [plugin check](#plugin-check), rejected rows, applying a commit, [notices](#notices), the `View` the overlay draws |
| `server/cmd/tradebot` | A scripted trade partner ([Trade bot](#trade-bot)) |

## Phases

| Phase | Meaning |
| --- | --- |
| `kIdle` | No trade |
| `kRequesting` | `TradeRequest` sent, no answer yet |
| `kInviting` | The server sent the local player's invitation; no answer yet |
| `kInvited` | Another player's invitation waits for the local player |
| `kOpen` | Both have the trade window |

A player is in at most one trade, invited or open, which the server enforces
(`BUSY`). An invitation lasts `trade.invite_seconds` (30) on the server; the
client ends a phase still waiting 5 s after that, or a request unanswered
after 10 s, as for a lost message.

## Invitation

- `Trade` in a player's menu is enabled when `PlayerDetails` found the
  player's character within `TradeSync::kRequestRadius` (400 units, horizontal,
  the server's default; an interior needs the same cell) when the menu opened,
  and no trade is under way. The server checks its own `trade.request_radius`
  against both players' latest checkpoints and refuses with `OUT_OF_RANGE`,
  `BUSY`, `UNAVAILABLE`, or `TRANSFORMED`, each a warning notice.
- A player in a [beast form](appearance-and-equipment.md#shapeshifts) has no
  inventory: the server refuses a request when either context has a
  shapeshift, and the local request is refused locally while transformed.
- Both players get the [prompt](ui-overlay.md#trade-window) and no notice. The
  invited one answers without losing input: `[Controls] AcceptTrade` (`Y`) and
  `DeclineTrade` (`N`), unless unbound, or the inviting player's menu. The
  inviting player's decline key withdraws it.
- Accepting is refused locally in combat, out of the world, or in a beast form.
- An invitation from a muted player is ignored without an answer, and the
  server never shows one from a player the invited player vanished: both
  expire for the sender, indistinguishable from an ignored one. A vanished
  player cannot be invited (`UNAVAILABLE`).

## Offers and acceptance

- `TradeInventory::Capture` lists the inventory when the trade opens: one row
  per distinct item (base form and extra data), counting all the player holds,
  so a worn piece and its carried copies are one row. Weapons and ammunition
  must be playable and not bound, armor playable, lights carriable; every
  alchemy item, ingredient, book, scroll, soul gem, key, and miscellaneous item,
  gold included, is listed. Rows that cannot be offered show why: a quest alias
  holds it, the player made it (a dynamic base form), or it cannot be described
  (an unreferenceable form, or a player enchantment with more than 4 effects or
  a dynamic effect).
- An offer holds at most 15 lines (`kMaximumTradeItems`), as many as its
  [grid](ui-overlay.md#trade-window) shows without scrolling, and at most
  15 KiB by `EstimateTradeItemBytes`. At 15 lines the rows not yet offered are
  disabled and a mark beside the own grid's heading shows why; an edit over the
  byte limit is refused with a warning notice.
- Edits are sent as a whole `TradeOffer` with the next revision 0.5 s of frame
  time (`kOfferBatchSeconds`) after the first unsent edit; later edits join
  that batch without delaying it. Any edit, and any new offer from the peer,
  clears both acceptances.
- Each received offer is resolved at once (`Forms::ResolveItem`) and answered
  with a `TradeReport` of the lines this game cannot receive: a missing plugin
  or form (`MISSING_ITEM`, with the plugin when it is not loaded), a form of a
  kind that cannot be traded (`WRONG_TYPE`), a missing enchantment or effect
  (`MISSING_EFFECT`), or an enchantment that is not one or on an item that
  takes none (`BAD_ENCHANTMENT`). The receiver never shows those lines.
- A clicked line shows in the own offer at once, not after the peer's
  confirmation, which would delay every click by a batch and a round trip; the
  [plugin check](#plugin-check) makes a line taken back rare. A line the peer's
  report skips leaves the offer, which sends a new revision. `TradeSync` keeps
  each row's check (`RowCheck`) for the trade, carried over when the inventory
  is listed again. A rejected row is disabled with its reason, and a warning
  notice per reason names the items that left the offer. Reasons have no final
  period, like [notices](ui-overlay.md#notices), since each is both the item
  card's warning and part of a notice:
  - the peer lacks the plugin, when the report names it; every other row using
    that plugin is disabled at once, and those already offered leave the offer
    and are named in the notice;
  - the peer has another version of the plugin (`MISSING_ITEM` without a
    plugin, `WRONG_TYPE`);
  - the peer's game lacks the enchantment (`MISSING_EFFECT` without a plugin)
    or cannot rebuild it (`BAD_ENCHANTMENT`).
- `Accept` is possible once the window holds something on either side, the own
  offer is sent, the peer reported on it, and this game reported on the
  peer's. It first checks that the player still holds every offered item; one
  gone (a script took it) refuses with a notice naming them and lists the
  inventory again. `TradeAccept` names both revisions; the server passes it to
  the peer in the peer's terms ([Network](network.md#trade)) or, when the
  peer's acceptance of the same pair stands, sends both a `TradeCommit`. An own
  revision with skipped lines is never accepted: the report that names them
  takes them out in the same pass, so Accept waits for the new revision's
  report.
- Each shown peer line has an id (`PeerLine::id`) kept while the peer offers
  the same `TradeItem`, count included, so the
  [preview column](ui-overlay.md#preview-column) can follow it.

## Plugin check

Before the player can offer a row, the peer's game says whether it has the
row's plugins, so an item from a mod the peer lacks is disabled before a click.

- When the trade opens and after each later listing, `TradeSync` collects the
  plugins of every offerable row (item, enchantment, effects), sorted and
  unique. `TradeSession::QueryPlugins` sends those not yet asked about as
  `TradePluginQuery`s, split at 256 names (`kMaximumTradePlugins`) or 15 KiB
  (`EstimateTradePluginBytes`). Only plugin names travel: the peer learns
  which mods the player's items come from, not what the player carries.
- The peer answers each query with a `TradePluginReply` naming the indices of
  plugins it has not loaded (`FormLookup::IsPluginLoaded`). The server relays
  replies in query order, so each matches the oldest unanswered query.
- Until its reply, a row with a queried plugin is disabled
  (`RowCheck::checking`) and the item list shows it is being checked.
- A missing plugin disables every row using it, as a report naming the plugin
  would, without a notice, since the player has not tried to offer anything.
- A reply missing after 5 s of frame time (`kPluginCheckSeconds`) releases the
  rows with a log warning; reports still check each line. A late reply still
  disables rows, and offered ones leave with the usual notice.
- The check knows only names: a plugin in another version passes, and the
  report catches its items after the click.

## Item details

Each inventory row and shown peer line carries `Forms::ItemDetails`, computed
on the game thread when the inventory is listed or the peer's offer is
resolved (`ReportPeerOffer`), never per frame. Both sides use the item as the
local game resolves it, so a peer's item shows the local plugins' numbers,
which is what the player would receive. The numbers are the item's own,
without either player's skills or perks; the card is in
[UI overlay](ui-overlay.md#trade-window).

- Damage, for a weapon other than a staff: `attackDamage × fDamageWeaponMult`
  plus the tempering bonus the game adds before skills (ID 26498), rounded.
  Ammunition: its damage.
- Armor, for armor with a rating: `ceil(armorRating / 100 + bonus)`, the bonus
  as ID 26499 computes it, doubled for armor with the `kKeywordCuirass` default
  object (Skyrim.esm `ArmorCuirass`, `0x0006C0EC`). Both bonuses are
  `max(0, progress × (fSmithingWeaponMax or fSmithingArmorMax − 1) + 1)`, where
  progress runs from 0 at `fHealthDataValue1` to 1 at `fHealthDataValue6`.
- Value, as `InventoryEntryData::GetValue` (ID 15995): the gold value scaled by
  tempering as ID 26483 does,
  `value × ((health − 1) / (fHealthDataValue6 − 1) + 1)`, rounded up; an
  enchantment adds its cost (`MagicItem::CalculateTotalGoldValue` with no
  caster), rounded, for a constant effect, or for a weapon
  `fEnchantmentPointsMult × maximum charge + value +
  fEnchantmentEffectPointsMult × cost` as in ID 26465. A player enchantment's
  cost is the sum of its effects' sent costs. The enchantment is the item's
  own, else the base form's (`TESEnchantableForm::formEnchanting`).
- The four formulas are ported rather than called, so they need no Address
  Library ID; the AE IDs resolve to unrelated functions on SE 1.5.97, where
  calling them crashed. Settings come from `GameSettingCollection`.
- Effects: of the enchantment, a player enchantment, or a potion, poison,
  scroll, or ingredient, each its `EffectSetting::magicItemDescription`
  resolved as the game's item card does (AE IDs 51901, 51905, 51906):
  - `<mag>`, `<dur>`, and `<area>` (any case) become numbers; other tags show
    their own text. A magnitude is truncated to an integer, after scaling by
    100 when the effect's `primaryAV` has
    `kDisplayedEffectMagnitudeTimesOneHundred`. `<global=EditorID>`, which no
    vanilla description uses, shows as text.
  - Every `[KEY=text]` section is dropped, including Survival Mode's `SURV`
    hunger text, in either mode.
  - An effect flagged `kHideInUI` or with no text left is left out.
  Ingredient effects the local player has not discovered
  (`IngredientItem::gamedata.knownEffectFlags`) show as unknown.
- Charge, for an enchanted weapon: the sent charge and maximum, or for a base
  form's enchantment the full `amountofEnchantment`.
- Weight is the base form's, at least 0 (`TESForm::GetWeight` returns -1 for a
  weightless form, such as ammo outside Survival Mode). The tempering tier is
  in the name: `Forms::GetDisplayName` adds it from the item's health with the
  game's `sHealthDataPrefixWeap1`–`6` or `sHealthDataPrefixArmo1`–`6` setting,
  so in the game's language.
- A row that cannot be offered shows its base form's details.

## Commit

A commit names the receiving player's revision and the peer's. `TradeSession`
keeps the last 16 revisions it sent, since the server may commit one an edit
in flight was about to replace; the commit applies that revision. Without the
peer's report on it nothing is given, since what the peer takes is unknown;
with one ordered stream that cannot happen and is logged as an error. Skipped
lines are left out on both sides as a guard.

1. Each own line the peer did not skip is removed: `TradeInventory::Remove`
   finds the stacks with the same item (base form, enchantment, charge,
   tempering, name, soul), carried before worn, and calls `RemoveItem` with no
   destination. A shortfall is logged.
2. Each peer line this game did not skip is resolved again and added. An item
   without extra data goes in with `AddObjectToContainer`. An item with extra
   data cannot have its stack built here, so each unit is placed as a reference
   (`PlaceObjectAtMe`), given its extra data and the player's base form as
   owner, and picked up (`PickUpObject`, no sound). Without the owner the
   reference takes its cell's owner, and picking it up in a shop or house
   would be theft. A player enchantment is rebuilt with
   `BGSCreatedObjectManager::AddWeaponEnchantment` or `AddArmorEnchantment` from
   its effects (magic effect, magnitude, area, duration, cost) and attached
   with its maximum charge; a weapon below it gets `ExtraCharge`. A plugin
   enchantment is attached as is.
3. `TradeInventory::PlayPickUpSound` plays the `kPickupSoundGeneric` default
   object's sound once at the player, on both sides.
4. A notice says the trade is done; what moved (entry counts given and
   received, each shortfall) is only in the log.

A pre-enchanted plugin weapon carries no `ExtraEnchantment`, so its charge is
not sent and it arrives full.

## Cancelling

| Event | Reason | Where |
| --- | --- | --- |
| Cancel, Escape | `CANCELLED` | client |
| The decline key or menu item | `DECLINED` | client (`TradeAnswer`) |
| Combat (`IsInCombat`) or a `TESHitEvent` on the player while invited or open | `COMBAT` | client |
| The local player in a beast form while invited or open | `TRANSFORMED` | client |
| A context with a shapeshift, of either player, while invited or open | `TRANSFORMED` | server |
| Anything taking input from the open window: a game menu, the console, dialogue, a loading screen; not the docked chat | `MENU` | client |
| The window cannot take input within 2 s of opening | `FAILED` | client |
| Over `trade.keep_radius` (1000) apart, another space, `Exit` | `OUT_OF_RANGE` | server |
| A disconnect | `DISCONNECTED` | server |
| No answer within `trade.invite_seconds` | `EXPIRED` | server |
| The local connection lost, or trade messages dropped from a full queue | — | client, locally |

A local cancel sends its message and ends the trade at once; the server's
close for it is ignored.

## Notices

Each outcome is a [notice](ui-overlay.md#notices) (`Notices::Post` from
`TradeSync`), not a chat line. Every end names the other player and the reason;
a commit has its own notice. A `TRANSFORMED` close names the local player's
own form when it is transformed, since the server may close before this game
cancels. Refusals of the player's own actions (a trade under way, combat, a
beast form, no connection, a full offer, items gone, an item the other game
cannot receive) are notices too. Sending or receiving an invitation posts
nothing: the [prompt](ui-overlay.md#prompt) shows both. All texts come from the
string tables.

## Trade bot

`cmd/tradebot` lets one player test trades against a scripted peer. It joins
as a real client with a session from the admin API (`--admin`, kept in
`<user cache>/playerhub/tradebot-<host>.json`), so it can only send what a
client may.

- It finds its target in the admin state (`GET /api/state`): the only other
  non-virtual player in the world, or `--target` by display name. It copies the
  target's space, appears `--distance` (150) units in front of it, and follows
  from relayed checkpoints, running at the target's speed or `--speed` (350)
  once 150 units farther than `--distance`. It jumps next to the target from
  another space or past 2500 units, using the admin state only after 1.5 s of
  relay silence, since it lags. A disconnected target is found again by name.
- It mirrors the plugin's side of the protocol: queries its offer's plugins
  when a trade opens, answers queries with the plugins its `skip plugin` rules
  name, reports on every offer at once, drops its own reported lines and sends
  a new revision, keeps the last 16 revisions for a commit, and prints what
  each commit gave and received; it has no inventory.
- `--auto-answer` and `--auto-accept` (both on) accept an invitation, and the
  exchange once the target accepted the current revisions. `--kit` starts with
  the test kit offered.
- The console (`help`) drives the rest: `invite`, `accept`, `decline`,
  `cancel` with any client reason; offer edits (`add` with every `TradeItem`
  field, `count`, `remove`, `clear`, `kit`), checked with `validate.TradeOffer`;
  `copy` and `mirror` to offer the target's lines back, which tests extra data
  both ways without knowing form IDs; `skip plugin` and `skip form` rules
  applied in `Forms::ResolveItem`'s order; and `follow`, `stay`, and `leave`
  (runs to 1500 units, past the default `trade.keep_radius`).
- The test kit is Skyrim.esm gold, lockpicks, an iron sword, a Superior steel
  sword (`health` 1.2), an iron dagger named `Bot Dagger`, iron arrows, and one
  item from `TradeBotMissing.esp`, which the receiver must skip naming its
  plugin.

It cannot test the peer's side of a commit, a peer with another modpack beyond
the reports it fakes, or combat and menus on the peer's side.

## Engine assumptions

1. `RemoveItem` with no destination on a worn or favorited stack unequips and
   removes it, and on a stack with extra data removes only that stack.
2. `AddWeaponEnchantment` and `AddArmorEnchantment` return the enchantment they
   already hold for the same effects, and it survives a save and reload.
3. `ExtraDataList::SetEnchantment`, `ExtraHealth`, `ExtraSoul`, `ExtraCharge`,
   and `SetOverrideName` on a placed reference survive `PickUpObject`.
4. `PickUpObject` takes a reference placed in the same frame, before its 3D
   loads; otherwise the item lies at the player's feet.
5. A unit picked up with the player's base form as owner keeps that
   `ExtraOwnership`, which the game does not count as stolen.
6. `TESHitEvent` arrives for every hit on the player, off the main loop.
7. The input contexts that release a focused window cover every menu a script
   can open, such as message boxes and NPC-started dialogue.
8. `ExtraTextDisplayData::customNameLength` is the given name's length before
   the tempering tier in `displayName`.
9. The [item details](#item-details) formulas of 1.7.104 IDs 26498, 26499,
   26483, and 26465 were read from the disassembly; SE's 25915, 25916, 25901,
   and 25893 read the same settings, default object, and constants. The ported
   numbers have not been compared with the game's item card.
10. `GetValue`'s enchantment lookup (ID 14563) falls back to the base form's
    enchantment, and a player enchantment's `CalculateTotalGoldValue` is the
    sum of its effects' costs.
11. A `BSSoundHandle` for the generic pickup descriptor, positioned at the
    player and following its 3D, is audible in first and third person.
12. A plugin in Data that is not active has compile index `0xFF` in
    `TESDataHandler::files`, if listed at all, so
    `FormLookup::IsPluginLoaded` reports it missing.

## Acceptance criteria

1. `Trade` in a player's menu is enabled only for a player standing near; the
   server refuses a far one with a notice.
2. The invited player sees the prompt, and no notice, without losing control;
   the keys and the menu answer it; it expires after 30 s; the sender is told
   each outcome.
3. Invitations from muted or vanished players never show and expire for the
   sender.
4. Both windows show both offers within a moment of an edit, and an edit takes
   both acceptances back.
5. A completed trade moves exactly the offered items and counts, with their
   enchantments, tempering, names, and souls, both ways, and nothing else.
6. Items using a plugin the receiver lacks are disabled with the reason,
   without a notice, a moment after the window opens, and show as being
   checked until then. A clicked item shows in the giver's offer at once and in
   the receiver's after the batch and a round trip. An item the receiver cannot
   receive for another reason never shows there and leaves the giver's offer
   with a notice.
7. Cancel, Escape, a disconnect, combat, a hit, any game menu, a loading
   screen, and walking apart each close both windows without changes.
8. Quest items and player-made potions show why they cannot be offered.

## Known limitations

- A committed trade reaches a client only while it stays connected: one that
  disconnects in that moment does not apply its side, while the other does.
- A player enchantment with more than 4 effects cannot be offered.
- Charge on a pre-enchanted plugin weapon is not kept.
- The client's request radius is the server's default; a server configured
  otherwise still decides.
- The item card shows the item's own numbers, not those after the player's
  skills and perks.
- Accept waits for the last edit's batch and report: 0.5 s and a round trip
  after the last click. A line the peer cannot receive despite having the
  plugin shows in the giver's offer for as long.
- An inventory needing more than 8 plugin queries (2048 names) waits out the
  5 s for queries past the server's pending limit.
- A receiver that accepts just before the giver's game drops a line it cannot
  receive has the acceptance taken back by the new revision, though its window
  did not change.
- The plugin check compares names only, so another version of a plugin is
  found one click at a time.
