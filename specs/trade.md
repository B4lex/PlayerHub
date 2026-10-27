# Trade

## Status

Built on Windows with the rest of the plugin. In game: the trade screen
opened and looked right ([UI overlay](ui-overlay.md#status)), the
inventory capture with item details ran on SE 1.5.97, and a click on a
single offered item removes it. A whole trade between two games, and the commit,
have not run in game.

- **Server:** `go test -race ./...` covers the hub's trades
  (`internal/hub`): invitation, opening, relayed offers, reports, plugin
  queries and replies, acceptances, and the commit with each player's
  revisions; acceptances cleared by an offer and refused for stale revisions
  or before their report; every request refusal, decline, cancel, a hidden
  invitation and its expiry, closing on moving apart, `Exit`, a disconnect,
  and a transformation; and the limit of pending plugin queries.
  `internal/validate` covers every item, report, query, reply, and cancel
  rule in [Network](network.md#trade).
- **Wire, on Linux with g++ 16:** `WireCodec.cpp` with the regenerated
  nanopb code decoded Go-encoded trade messages of every kind, including
  player and plugin enchantments, tempering, names, and plugin queries up to
  their limits, and rejected their invalid forms; the messages it encoded
  passed the server's `validate.Message`, and it encoded nothing past the
  item, plugin, and byte limits. The line and report limits were checked
  at 32, before they became 15. `EstimateTradeItemBytes` stayed above the
  encoded size, 1664 against 1580 bytes for the largest valid item.
- **Session:** `TradeSession.cpp` ran 156 checks: every phase change, the
  batch interval before an offer is sent, acceptance blockers and
  acceptances taken back, a commit naming an older revision or arriving
  without its report, the 16 kept revisions, refusals, crossing
  invitations, timeouts, and the plugin check's queries, replies, and
  timeout. All but the plugin check's 37 predate offer lines shown at once,
  and none covers the beast form refusal.
- **Windows and engine:** `TradeWindow.cpp`, `TradePrompt.cpp`, and the menu
  entry ran in headless Dear ImGui harnesses ([UI overlay](ui-overlay.md#status)).
  The [item details](#item-details) formulas and the [commit](#commit)'s
  ownership calls were read from the 1.7.104 disassembly and compared with
  their 1.5.97 counterparts (25915, 25916, 25901, 25893); no number was
  compared with the game's. `TradeSync`, `TradeInventory`, and
  `ItemInstance` have not run outside the game, and their engine calls have
  not run in it.
- **[Trade bot](#trade-bot):** two bots and an idle `cmd/loadgen` walker
  traded with each other on a local server: an invitation answered at once,
  offers with tempering, names, and a player enchantment, a plugin query
  answered with `TradeBotMissing.esp` missing ahead of the report, a
  `skip plugin` report that made the giver drop the line and send a new
  revision, both acceptances and the commit, an edit after an acceptance,
  `cancel menu`, a decline, `leave` closing the trade with `OUT_OF_RANGE`,
  and a quit closing it with `DISCONNECTED`. Not run against the game.
- **Player-made potions and poisons:** `go test -race ./...` covers their
  `validate` rules. The plugin side has not been compiled, the bot's
  `add` without a form has not run, and nothing has run on Windows or in
  game.

## Scope

Two nearby players exchange inventory items. One offers a trade from the
other's [player menu](ui-overlay.md#player-menu); the other accepts from a
prompt; both put items from their inventories into the exchange and accept.
When both accepted the same offers, each game removes what its player gave
and adds what its player received.

This is the only feature through which a peer changes the local world, and
only the local player's inventory, only by an exchange the local player
accepted. The server relays and orders the exchange and checks range; each
client resolves the peer's items against its own plugins, so players with
different modpacks trade what both have.

Goal: an exchange of any items both games know, kept whole: what one player
gives the other receives, with its enchantment, tempering, name, and soul,
and nothing the receiver lacks leaves the giver.

Non-goals:

- Server authority over inventories, anti-cheat, and protection from a
  modified client or a reloaded save. A trade cannot be undone and the game
  does not save after one, so a player who quits without saving loses what
  it received while the other side stands (the user's choice).
- Poisons applied to a weapon, and any dynamic form other than a player
  enchantment or a player-made potion or poison, which are rebuilt from their
  effects. Rebuilding other forms, such as a cloned weapon, would need the
  whole record on the wire and a store the vanilla game lacks.
- Trading with a player who is not near, a replay, or an in-server bot.
  The [trade bot](#trade-bot) is a real client and can.
- Ownership: the giver's stolen mark is not carried; a received item is
  never stolen.
- Showing the trade on the characters (facing each other, an idle).

The wire messages and their validation are in
[Network](network.md#trade); the prompt, the window, and the menu entry in
[UI overlay](ui-overlay.md#trade-window); the stage's place in the pass in
[Sync pipeline](sync-pipeline.md#frame-sequence); the keys in
[Configuration](configuration.md#controls).

## Components

| Module | Responsibility |
| --- | --- |
| `protocol/TradeState.h` | `TradeItem`, `TradeSkip`, close reasons, the events of a trade, their validators and size limits, `EstimateTradeItemBytes` |
| `server/internal/hub/trade.go` | A trade's two players, its phase, both offer revisions, item counts, reports, and acceptances; range, busy, vanish, expiry, and commit |
| `forms/ItemInstance` | An inventory stack to a `TradeItem` or the reason it cannot be offered; a `TradeItem` to local forms or a skip reason; rebuilding a player enchantment or a player-made potion or poison; putting extra data on a reference |
| `forms/ItemDetails` | A resolved item's [details](#item-details) for the trade window's item card: damage or armor, weight, value, effects, charge |
| `sync/trade/TradeSession` | The state machine of one trade, without engine types: phases, the own offer and the revisions sent, the peer's offer, both reports, the plugin queries sent and their replies, acceptances, and what a commit applies |
| `sync/trade/TradeInventory` | The player's offerable inventory, how many of an item it holds, removing and adding items |
| `server/cmd/tradebot` | A scripted trade partner for testing alone ([Trade bot](#trade-bot)) |
| `sync/trade/TradeSync` | The game-thread stage: trade events and the overlay's commands, cancelling on combat or a hit, sending offers in batches, the [plugin check](#plugin-check) both ways, which rows the peer's game rejected, applying a commit, [notices](#notices), and the `View` the overlay draws |

## Phases

| Phase | Meaning |
| --- | --- |
| `kIdle` | No trade |
| `kRequesting` | `TradeRequest` sent, no answer yet |
| `kInviting` | The server sent the local player's invitation; the other player has not answered |
| `kInvited` | Another player's invitation waits for the local player |
| `kOpen` | Both have the trade window |

A player is in at most one trade, invited or open, which the server enforces
(`BUSY`). An invitation lasts `trade.invite_seconds` (30) on the server; the
client shows the time left and ends a phase still waiting 5 s after that, or
a request unanswered after 10 s, as for a lost message.

## Invitation

- `Trade` in a player's menu is enabled when `PlayerDetails` found the
  player's character within `TradeSync::kRequestRadius` (400 units,
  horizontal, the server's default; an interior needs the same cell) when the
  menu opened, and no trade is under way. The server checks again against its
  own `trade.request_radius` from both players' latest checkpoints, and
  refuses with `OUT_OF_RANGE`, `BUSY`, `UNAVAILABLE`, or `TRANSFORMED`, each a
  warning notice.
- A player in a [beast form](appearance-and-equipment.md#shapeshifts) has no
  inventory to trade from: the server refuses a request when either player's
  context has a shapeshift, and the local player's own request is refused
  locally, with a notice, while it is transformed.
- Both players get the [prompt](ui-overlay.md#trade-window), and no notice,
  since the prompt already says it. The invited one answers without losing
  input:
  `[Controls] AcceptTrade` (`Y`) accepts and `DeclineTrade` (`N`) declines,
  unless unbound, and its menu for the inviting player offers accept and
  decline items. The inviting player's prompt waits, and the decline key
  withdraws it.
- Accepting is refused locally, with a notice, in combat, out of
  the world, or in a beast form.
- An invitation from a muted player is ignored without an answer, and the
  server never shows one from a player the invited player vanished: both
  expire for the sender as if unanswered, so neither tells it apart from a
  player who ignored it. A vanished player cannot be invited either
  (`UNAVAILABLE`).

## Offers and acceptance

- `TradeInventory::Capture` lists the player's inventory when the trade
  opens: one row per distinct item (base form and extra data), counting all
  of it the player holds, so a worn piece and its carried copies are one row.
  Weapons and ammunition must be playable and not bound, armor playable, and
  lights carriable; every alchemy item, ingredient, book, scroll, soul gem,
  key, and miscellaneous item, gold included, is listed. Rows that cannot be
  offered show why: a quest needs it (an alias holds it), the player made it
  (a dynamic base form other than a potion or poison), or it cannot be described
  (an unreferenceable form, or a player enchantment with more than 4 effects
  or one from a dynamic effect).
- A player-made potion or poison (a dynamic `AlchemyItem`) is offered as its
  effects without an `item`, read as a player enchantment's are, so a line with
  more than 4 effects, one from a dynamic effect, or values past the
  [wire limits](network.md#trade) cannot be offered. Its name
  and its card are built from the effects on both sides
  ([item details](#item-details)).
- An offer holds at most 15 lines (`kMaximumTradeItems`), as many as its
  [grid](ui-overlay.md#trade-window) shows without scrolling, and at most
  15 KiB by `EstimateTradeItemBytes`. A line past the 15th cannot be
  added: the rows not yet offered are disabled without a notice, and a
  mark beside the own grid's heading shows why (the user's choice); an
  edit over the byte limit is refused with a warning notice.
- Edits are sent as a whole `TradeOffer` with the next revision 0.5 s of
  frame time after the first edit not yet sent (`kOfferBatchSeconds`, the
  user's interval). Later edits join that batch without delaying it, so
  steady clicking cannot hold an offer back. Any edit, and any new offer
  from the peer, clears both players' acceptances.
- Each received offer is resolved at once (`Forms::ResolveItem`) and answered
  with a `TradeReport` of the lines this game cannot receive: an item whose
  plugin or form is missing (`MISSING_ITEM`, with the plugin when it is not
  loaded), a form of a kind that cannot be traded (`WRONG_TYPE`), an
  enchantment or an effect whose plugin or form is missing
  (`MISSING_EFFECT`), or an enchantment that is not one, or on an item that
  takes none (`BAD_ENCHANTMENT`). The receiver never shows those lines.
- A clicked line shows in the own offer at once (the user's choice), rather
  than once the peer's game confirms it, which would delay every click by a
  batch and a round trip; the [plugin check](#plugin-check) disables rows
  from plugins the peer lacks before the player clicks, so a line taken back
  is rare. A line the peer's report skips leaves the
  offer, which sends a new revision. `TradeSync` keeps each inventory row's
  check (`RowCheck`) for the trade; rejections and missing plugins carry
  over when the inventory is listed again. A rejected row is disabled with
  the reason, and a warning notice per reason names the items that left
  the offer and why. The reasons end without a final period, like
  [notices](ui-overlay.md#notices), since each is both the item card's
  warning and part of a notice:
  - the peer lacks the plugin, when the report names it; every other row whose item, enchantment, or effect comes from
    that plugin is disabled at once with the same reason, and those already
    in the offer leave it and are named in the notice.
  - the peer has another version of the plugin, for a missing form in a
    loaded plugin, or a form of another kind (`MISSING_ITEM` without a
    plugin, `WRONG_TYPE`).
  - the peer's game lacks the enchantment (`MISSING_EFFECT` without a
    plugin) or cannot rebuild it (`BAD_ENCHANTMENT`).
- `Accept` is possible once the window holds something on either side, the
  own offer is sent, the peer reported on it, this game reported on the
  peer's, and the player has had the cursor over each of the peer's lines
  since the peer added or changed it
  ([UI overlay](ui-overlay.md#trade-window), as Path of Exile asks, so a
  swapped item cannot pass unseen). `TradeSync` gives each shown peer line
  an id (`PeerLine::id`) that it keeps while the peer offers the same item
  (`TradeItem` equal, count included). It first checks that the player still holds every offered item;
  one gone (a script took it) refuses with a notice naming them and
  lists the inventory again. `TradeAccept` names both revisions; the server
  passes it on to the peer in the peer's terms
  ([Network](network.md#trade)), or, when the peer's acceptance of the same pair
  stands, sends both a `TradeCommit`. A player never accepts an own revision
  with skipped lines: the report that names them takes them out of the
  offer in the same pass, so Accept waits for the new revision's report.

## Plugin check

Before the player can offer a row, the peer's game says whether it has the
row's plugins, so an item from a mod the peer lacks is disabled ahead of a
click rather than shown and taken back.

- When the trade opens, and after each later inventory listing, `TradeSync`
  collects the plugins of every offerable row (its item, enchantment, and
  effects), sorted and without duplicates. `TradeSession::QueryPlugins`
  sends those not asked about in this trade as `TradePluginQuery`s, split at
  256 names (`kMaximumTradePlugins`) or 15 KiB by `EstimateTradePluginBytes`.
  Only plugin names travel (the user's choice): the peer learns which mods
  the player's items come from, not what the player carries.
- The peer's game answers each query with a `TradePluginReply` naming the
  indices of the plugins it has not loaded (`FormLookup::IsPluginLoaded`).
  The server relays replies in query order, so `TradeSession` matches each
  to its oldest unanswered query.
- Until its reply, a row with a plugin in an unanswered query is disabled
  (`RowCheck::checking`), and the item list shows that it is being checked
  ([UI overlay](ui-overlay.md#trade-window)), usually for one round trip
  after the window opens.
- A missing plugin disables every row that uses it with the reason that the
  peer lacks it, as a report naming the plugin would, and the reason carries
  over later listings. No notice is posted, since the player has not tried
  to offer anything (the user's choice).
- A reply that has not come within 5 s of frame time
  (`kPluginCheckSeconds`) releases the rows, with a warning in the log; the
  offer's report still checks each line. A late reply still disables
  rows, and those already offered leave the offer with the usual notice.
- The check knows only names: a plugin the peer has in another version
  passes, and the report catches its items after the click.

## Item details

Each inventory row and each shown peer line carries `Forms::ItemDetails`,
computed on the game thread when the inventory is listed
(`TradeInventory::Capture`) or the peer's offer is resolved
(`ReportPeerOffer`), never per frame. Both sides use the same function on
the item as the local game resolves it, so a peer's item shows the local
plugins' numbers, which is what the player would receive. The numbers are
the item's own, without either player's skills or perks (the user's
choice), and the item card that shows them is in
[UI overlay](ui-overlay.md#trade-window).

- Damage, for a weapon other than a staff:
  `attackDamage × fDamageWeaponMult` plus the tempering bonus the game adds
  before skills (1.7.104 ID 26498), rounded. Ammunition: its damage.
- Armor, for armor with a rating: `ceil(armorRating / 100 + bonus)`, the
  bonus as ID 26499 computes it, doubled for armor with the
  `kKeywordCuirass` default object, matched as Skyrim.esm `ArmorCuirass`
  (`0x0006C0EC`). Both bonuses are
  `max(0, progress × (fSmithingWeaponMax or fSmithingArmorMax − 1) + 1)`,
  where progress runs from 0 at `fHealthDataValue1` to 1 at
  `fHealthDataValue6`, so they are 0 untempered.
- Value, as `InventoryEntryData::GetValue` (ID 15995) computes it: the gold
  value scaled by tempering as ID 26483 does,
  `value × ((health − 1) / (fHealthDataValue6 − 1) + 1)`, and rounded up; an
  enchantment adds its cost (`MagicItem::CalculateTotalGoldValue` with no
  caster), rounded, for a constant effect, or for a weapon enchantment is
  `fEnchantmentPointsMult × maximum charge + value +
  fEnchantmentEffectPointsMult × cost` as in ID 26465. A player
  enchantment's cost is the sum of its effects' sent costs. The enchantment
  is the item's own, else the base form's
  (`TESEnchantableForm::formEnchanting`).
- The four formulas are ported rather than called, so they need no
  Address Library ID on either runtime; the AE IDs resolve to unrelated
  functions on SE 1.5.97, where calling them crashed the inventory capture.
  The game settings come from `GameSettingCollection`, so the numbers
  follow each runtime's settings.
- Effects: of the enchantment, a player enchantment's effects, or a potion,
  poison, scroll, or ingredient, each its effect's item description
  (`EffectSetting::magicItemDescription`) with `<mag>`, `<dur>`, and `<area>`
  filled (any case; magnitudes whole unless fractional), or its name when it
  has none. An ingredient's effects the local player has not discovered
  (`IngredientItem::gamedata.knownEffectFlags`) are shown as unknown, as in
  the game (the user's choice).
- Charge, for an enchanted weapon: the sent charge and maximum, or for a
  base form's enchantment the full `amountofEnchantment`, which is what
  arrives.
- Weight is the base form's, at least 0: `TESForm::GetWeight` returns -1
  for a form it counts as weightless, such as ammo outside Survival Mode.
  The tempering tier is already in the name: `Forms::GetDisplayName` adds
  it in parentheses from the item's health, named by the game's
  `sHealthDataPrefixWeap1`–`6` or `sHealthDataPrefixArmo1`–`6` setting, so in
  the game's language ([Localization](ui-overlay.md#localization)).
- A row that cannot be offered has no instance data, so its details are its
  base form's.
- A player-made potion or poison has no local form until it is received, so its
  card has its effects and, as value, their summed costs, rounded; no weight.
  `Forms::GetDisplayName` names it after its costliest effect, and its line has
  no preview on the receiver's side.

## Commit

A commit names the receiving player's revision and the peer's.
`TradeSession` keeps the last 16 revisions it sent, since the server may
commit one an edit in flight was about to replace: a commit then applies
that revision, which is what the peer accepted. Without the peer's report on
that revision it gives nothing, since what the peer takes is unknown and
keeping the items loses nothing; that cannot happen with one ordered stream
and is logged as an error.

Skipped lines are still left out on both sides, as a guard; an accepted
revision holds none.

1. Each own line the peer did not skip is removed: `TradeInventory::Remove`
   finds the stacks with the same item again (base form, enchantment,
   charge, tempering, name, soul), carried ones before worn ones, and calls
   `RemoveItem` with no destination. A shortfall is logged.
2. Each peer line this game did not skip is resolved again and added. An
   item without extra data goes in with `AddObjectToContainer`. An item with
   extra data cannot have its stack built here, so each unit is placed as a
   reference (`PlaceObjectAtMe`), given its extra data and the player's
   base form as owner, and picked up (`PickUpObject`, no sound). Without
   the owner the reference takes its cell's owner, and picking it up in a
   shop or a house is a theft: a bounty when seen, a stolen item otherwise.
   A player enchantment is rebuilt with
   `BGSCreatedObjectManager::AddWeaponEnchantment` or
   `AddArmorEnchantment` from its effects (magic effect, magnitude, area,
   duration, cost) and attached with its maximum charge; a weapon below it
   gets `ExtraCharge`. A plugin enchantment is attached as it is.
   A player-made potion or poison is created from its effects
   (`Forms::CreateAlchemyItem`): with `AddPoison` when the giver's was a
   poison (`poison`, from `AlchemyItem::IsPoison`), else `AddPotion`, and added with
   `AddObjectToContainer` while its `CreatedObjPtr` is held. `AddPoison` is
   called through its own relocation (35266, 36168), since CommonLib declares
   its result with a smart pointer `AlchemyItem` cannot use.
3. `TradeInventory::PlayPickUpSound` plays the generic pickup sound (the
   `kPickupSoundGeneric` default object, so a modpack's override applies) at the
   player, on both sides and whatever moved. The single unit pickups in
   step 2 stay silent so it plays once.
4. A notice says the trade is done; what
   moved is in the log (counts of entries given and received, and each
   shortfall), not the notice, which the user chose to keep short.

A pre-enchanted plugin weapon carries no `ExtraEnchantment`, so its charge
is not sent and it arrives full.

## Cancelling

| Event | Reason | Where |
| --- | --- | --- |
| Cancel, Escape | `CANCELLED` | client |
| The decline key or menu item | `DECLINED` | client (`TradeAnswer`) |
| Combat (`IsInCombat`) or a `TESHitEvent` on the player while a trade is invited or open | `COMBAT` | client |
| The local player in a beast form while a trade is invited or open | `TRANSFORMED` | client |
| A context with a shapeshift, of either player, while invited or open | `TRANSFORMED` | server |
| Anything taking input from the open window: a game menu, the console, dialogue, a loading screen | `MENU` | client |
| The window cannot take input within 2 s of opening | `FAILED` | client |
| Over `trade.keep_radius` (1000) apart, another space, `Exit` | `OUT_OF_RANGE` | server |
| A disconnect | `DISCONNECTED` | server |
| No answer within `trade.invite_seconds` | `EXPIRED` | server |
| The local connection lost, or trade messages dropped from a full queue | — | client, locally |

A local cancel sends its message and ends the trade at once; the server's
close for it is then ignored.

## Notices

Each outcome reaches the player as a [notice](ui-overlay.md#notices)
(`Notices::Post` from `TradeSync`), not a chat line. Every end of a trade
names the other player and why (declined, busy, unavailable, too far, no
answer, combat, a beast form, withdrawn or cancelled, left, could not open,
the connection lost, or the player's own decline); a commit has a notice of
its own. A `TRANSFORMED` close names the local player's own form when it is
transformed, since the server may close for it before this game cancels.
Refusals of the player's own actions (a trade
already under way, combat, a beast form, no connection, a full offer, items
gone, an item the other game cannot receive) are notices too. Sending an invitation and receiving one post nothing:
the [prompt](ui-overlay.md#prompt) shows both. The notices, the reasons a row cannot be offered, and
the Accept button's tooltips come from the string tables, so they are in the
game's language.

## Trade bot

`cmd/tradebot` lets one player test trades from the game against a
scripted peer. It joins as a real client with a session from the admin API
(`--admin`, kept in `<user cache>/playerhub/tradebot-<host>.json`), so the
server treats it as any player, and it can only send what a client may.

- It finds its target in the admin state (`GET /api/state`): the only other
  non-virtual player in the world, or `--target` by display name. It copies
  the target's space, appears `--distance` (150) units in front of it, and
  follows from the relayed checkpoints, running at the target's speed or
  `--speed` (350) once the target is 150 units farther than `--distance`.
  It jumps next to the target from another space or past 2500 units. The
  admin state places the target only while the relay has been quiet for
  1.5 s, since it lags. A target that disconnects is chosen again by name.
- It mirrors the plugin's side of the protocol: it asks about the plugins
  of its offer when a trade opens and prints the reply, answers a plugin
  query with the plugins its `skip plugin` rules name, reports on every offer
  at once, and on a report naming its own lines it drops them and sends a
  new revision, so it never accepts a revision with skipped lines. It keeps
  the last 16 revisions it sent for a commit that names an older one, and
  prints what each commit gave and received; it has no inventory.
- `--auto-answer` and `--auto-accept` (both on) accept an invitation, and
  the exchange once the target accepted the current revisions. `--kit`
  starts with the test kit in its offer.
- The console drives everything else (`help`): `invite`, `accept`,
  `decline`, `cancel` with any reason a client may send; offer edits (`add`
  with every `TradeItem` field, without a form for a player-made item,
  `count`, `remove`, `clear`, `kit`), checked with `validate.TradeOffer` and
  sent at once while a trade is open;
  `copy` and `mirror` to offer the target's own lines back, which tests
  extra data both ways without knowing form IDs; `skip plugin` and
  `skip form` rules that report lines as a game lacking that plugin or form
  would (the order of `Forms::ResolveItem`); and `follow`, `stay`, and
  `leave`, which runs to 1500 units, past the default `trade.keep_radius`.
- The test kit is Skyrim.esm gold, lockpicks, an iron sword, a Superior
  steel sword (`health` 1.2), an iron dagger named `Bot Dagger`, iron arrows,
  and one item from `TradeBotMissing.esp`, which the receiver must skip with
  its plugin named.

What it cannot test: the peer's side of a commit (its inventory), a peer
with another modpack beyond the reports it fakes, and combat or menus on
the peer's side.

## Engine assumptions

To verify in game:

1. `RemoveItem` with no destination on a worn or favorited stack unequips it
   and removes it, and on a stack with extra data removes only that stack.
2. `AddWeaponEnchantment` and `AddArmorEnchantment` return the enchantment
   they already hold for the same effects, as enchanting at a table does,
   and the enchantment survives a save and a reload.
3. `ExtraDataList::SetEnchantment`, `ExtraHealth`, `ExtraSoul`, `ExtraCharge`,
   and `SetOverrideName` on a placed reference survive `PickUpObject`, and
   the inventory shows the tempering tier and the given name.
4. `PickUpObject` takes a reference placed in the same frame, before its 3D
   loads. If not, the item lies at the player's feet.
5. A unit picked up with the player's base form as owner keeps that
   `ExtraOwnership` in the inventory, which the game does not count as
   stolen, so it can be sold to a merchant without a fence.
6. `TESHitEvent` arrives for every hit on the player, on a thread other than
   the main loop.
7. The input contexts that already release a focused window cover every menu
   a script can open, such as message boxes and dialogue started by an NPC.
8. `ExtraTextDisplayData::customNameLength` is the length of the given name
   before the tempering tier in `displayName`.
9. [Item details](#item-details): the formulas of 1.7.104 IDs 26498, 26499,
   26483, and 26465 were read from the disassembly, and SE 1.5.97's 25915,
   25916, 25901, and 25893 read the same settings, default object, and
   constants; `kKeywordCuirass` is `ArmorCuirass` in every load order.
10. `GetValue`'s enchantment lookup (ID 14563) returns the base form's own
   enchantment when the stack has none, so a pre-enchanted item's value
   includes it; and a player enchantment's `CalculateTotalGoldValue` is the
   sum of its effects' costs.
11. A `BSSoundHandle` from `BSAudioManager::GetSoundHandle` for the generic
   pickup descriptor, positioned at the player and following the player's
   3D, plays audibly in first and third person.
12. A plugin in Data that is not active has compile index `0xFF` in
   `TESDataHandler::files`, if that list holds it at all, so
   `FormLookup::IsPluginLoaded` reports it missing to the
   [plugin check](#plugin-check) and in a report's `plugin`.
13. `AddPotion` and `AddPoison` return the potion or poison the manager
   already holds for the same effects, as brewing does, and name it. An item
   added to an inventory keeps its own manager reference after the
   `CreatedObjPtr` is released, and survives a save and a reload.
14. `AddPoison` fills a `CreatedObjPtr<AlchemyItem>` as `AddPotion` does.

## Acceptance criteria

1. `Trade` in a player's menu is enabled only for a player standing near;
   the server refuses a far one with a notice.
2. The invited player sees the prompt, and no notice, without losing control;
   the keys and the menu answer it; it expires after 30 s; the sender is told
   each outcome.
3. Invitations from muted or vanished players never show and expire for the
   sender.
4. Both windows show both offers within a moment of an edit, and an edit
   takes both acceptances back. A line the peer adds or changes keeps Accept
   disabled until the player hovers it.
5. A completed trade moves exactly the offered items and counts, with their
   enchantments, tempering, names, and souls, both ways, and nothing else.
6. Items, and items with an enchantment or effect, from a plugin the
   receiver lacks are disabled with the reason, without a notice, a moment
   after the window opens; until then the item list shows it is checking
   and its rows are disabled. A clicked item shows in the giver's offer at
   once and in the receiver's after the batch and a round trip. An item the
   receiver cannot receive for another reason, such as another version of
   its plugin, never shows in the receiver's window and leaves the giver's
   offer with a notice.
7. Cancel, Escape, a disconnect, combat, a hit, any game menu, a loading
   screen, and walking apart each close both windows without changes.
8. Quest items show why they cannot be offered.
9. A player-made potion or poison arrives with the giver's effects, as a
   potion or a poison as the giver had it, stacks with an identical one the
   receiver brews, and survives a save and a reload.

## Known limitations

- A trade the server committed reaches a client only while it stays
  connected: one that disconnects in that moment does not apply its side,
  while the other does.
- A player enchantment with more than 4 effects cannot be offered.
- Charge on a pre-enchanted plugin weapon is not kept.
- The client's request radius is the server's default; a server configured
  wider or narrower still decides, and the menu may enable or disable
  `Trade` against it.
- The item card shows the item's own damage, armor, and value, not the numbers
  the inventory shows after the player's skills and perks.
- Accept waits for the last edit's batch and its report: 0.5 s and a round
  trip after the last click.
- A line the peer's game cannot receive although it has the plugin shows in
  the giver's offer until the report on its batch, 0.5 s and a round trip
  after the click.
- An inventory with plugins for more than 8 queries (2048 names) waits out
  the 5 s for the queries past the server's pending limit.
- A receiver that accepts in the moment before the giver's game drops a
  line it cannot receive has the acceptance taken back by the new revision,
  though its window did not change: the server clears both acceptances on
  every offer.
- The plugin check compares names only: an item from a plugin the peer has,
  but in another version, is found one click at a time.
