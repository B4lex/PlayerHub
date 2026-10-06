# Trade

## Status

Implemented 2026-10-06; not built on Windows and not run in game.

- Server: `go test -race ./...` covers the hub's trades (`internal/hub`): an
  invitation to both players, opening, offers relayed, a report relayed, an
  acceptance relayed and a commit to both with each one's revisions; an offer
  clearing both acceptances; an acceptance refused for stale revisions or
  before its report; a stale, duplicate, or out-of-range report; the request's
  refusals (`UNAVAILABLE` for oneself, an unknown slot, and the rate limit,
  `OUT_OF_RANGE`, `BUSY` for either player); decline and cancel; an invitation
  hidden from a player who vanished the requester and its expiry; closing on
  moving apart, `Exit`, and a disconnect. `internal/validate` covers every
  item, report, and cancel rule in [Network](network.md#trade).
- Plugin, on Linux with g++ 16:
  - `WireCodec.cpp` with the regenerated nanopb code decoded Go-encoded trade
    messages (an invitation, an opening, an offer of three items with player
    and plugin enchantments, tempering, a name, a report, an acceptance, a
    commit, closes with and without a trade) and rejected an invitation
    without a recipient, an opening without a trade, offers with 33 items, a
    soul above grand, and an enchantment with effects, a report index of 32 or
    an unknown reason, and closes without or with an unknown reason. Its
    encoded request, answer, offer, report, acceptance, and cancel passed the
    server's `validate.Message`. `EstimateTradeItemBytes` stayed above the
    encoded size, 1664 against 1580 bytes for the largest valid item.
  - `TradeSession.cpp` ran 119 checks: every phase change, the quiet interval
    before an offer is sent, acceptance blockers, acceptances taken back by
    an edit or a new peer offer, a commit naming a revision older than the
    newest sent one, a commit without its report keeping the items, the
    16 kept revisions, refusals, an invitation crossing the local request,
    and the timeouts.
  - `TradeWindow.cpp`, `TradePrompt.cpp`, `ChatPlayerMenu.cpp`,
    `WindowManager.cpp`, and `Controls.cpp` ran in a headless Dear ImGui
    1.92.9 context against a stubbed `TradeSync` and configuration (47
    checks), and the window and prompt were rasterized at 1080p in Dear
    ImGui's default style: see [UI overlay](ui-overlay.md#trade-window).
  - `ItemInstance.cpp`, `TradeInventory.cpp`, `TradeSync.cpp`, and the
    changed `PlayerDetails.cpp`, `SyncCoordinator.cpp`, `PluginStartup.cpp`,
    `ServerClient.cpp`, and `SettingsSchema.cpp` passed a g++ syntax check
    against the CommonLibSSE-NG headers, whose own Windows layout assertions
    fail under g++ and were ignored. None of the engine calls ran.

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
- Player-made potions and poisons, poisons applied to a weapon, and any other
  dynamic form except a player enchantment, which is rebuilt from its effects.
- Trading with a player who is not near, a replay, or a bot.
- Ownership: received items are unowned, also stolen ones.
- Showing the trade on the characters (facing each other, an idle).

The wire messages and their validation are in
[Network](network.md#trade); the prompt, the window, and the menu entry in
[UI overlay](ui-overlay.md#trade-window); the stage's place in the pass in
[Sync pipeline](sync-pipeline.md#frame-sequence); the keys in
[Configuration](configuration.md#ui).

## Components

| Module | Responsibility |
| --- | --- |
| `protocol/TradeState.h` | `TradeItem`, `TradeSkip`, close reasons, the events of a trade, their validators and size limits, `EstimateTradeItemBytes` |
| `server/internal/hub/trade.go` | A trade's two players, its phase, both offer revisions, item counts, reports, and acceptances; range, busy, vanish, expiry, and commit |
| `forms/ItemInstance` | An inventory stack to a `TradeItem` or the reason it cannot be offered; a `TradeItem` to local forms or a skip reason; rebuilding a player enchantment; putting extra data on a reference |
| `sync/trade/TradeSession` | The state machine of one trade, without engine types: phases, the own offer and the revisions sent, the peer's offer, both reports, acceptances, and what a commit applies |
| `sync/trade/TradeInventory` | The player's offerable inventory, how many of an item it holds, removing and adding items |
| `sync/trade/TradeSync` | The game-thread stage: trade events and the overlay's commands, cancelling on combat or a hit, sending offers once edits are quiet, applying a commit, notices, and the `View` the overlay draws |

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
  refuses with `OUT_OF_RANGE`, `BUSY`, or `UNAVAILABLE`, each told in the
  chat.
- Both players get a chat notice. The invited one gets the
  [prompt](ui-overlay.md#trade-window) without losing input:
  `[UI] TradeAcceptKey` (`Y`) accepts and `TradeDeclineKey` (`N`) declines,
  and its menu for the inviting player offers `Accept trade` and
  `Decline trade`. The inviting player's prompt waits, and the decline key
  withdraws it.
- Accepting is refused locally, with a notice, in combat or out of the
  world.
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
  (a dynamic base form, such as a brewed potion), or it cannot be described
  (an unreferenceable form, or a player enchantment with more than 4 effects
  or one from a dynamic effect).
- An offer holds at most 32 lines and at most 15 KiB by
  `EstimateTradeItemBytes`; an edit over either is refused with a notice.
- Edits are sent as a whole `TradeOffer` with the next revision once none
  came for 0.2 s of frame time. Any edit, and any new offer from the peer,
  clears both players' acceptances.
- Each received offer is resolved at once (`Forms::ResolveItem`) and answered
  with a `TradeReport` of the lines this game cannot receive: an item whose
  plugin or form is missing (`MISSING_ITEM`, with the plugin when it is not
  loaded), a form of a kind that cannot be traded (`WRONG_TYPE`), an
  enchantment or an effect whose plugin or form is missing
  (`MISSING_EFFECT`), or an enchantment that is not one, or on an item that
  takes none (`BAD_ENCHANTMENT`). Both windows mark those lines with the
  reason; they are skipped, not refused: the giver keeps them and the rest is
  exchanged (the user's choice).
- `Accept` is possible once the window holds something on either side, the
  own offer is sent, the peer reported on it, and this game reported on the
  peer's. It first checks that the player still holds every offered item;
  one gone (a script took it) refuses with a notice and lists the inventory
  again. `TradeAccept` names both revisions; the server relays it to the
  peer, or, when the peer's acceptance of the same pair stands, sends both a
  `TradeCommit`.

## Commit

A commit names the receiving player's revision and the peer's.
`TradeSession` keeps the last 16 revisions it sent, since the server may
commit one an edit in flight was about to replace: a commit then applies
that revision, which is what the peer accepted. Without the peer's report on
that revision it gives nothing, since what the peer takes is unknown and
keeping the items loses nothing; that cannot happen with one ordered stream
and is logged as an error.

1. Each own line the peer did not skip is removed: `TradeInventory::Remove`
   finds the stacks with the same item again (base form, enchantment,
   charge, tempering, name, soul), carried ones before worn ones, and calls
   `RemoveItem` with no destination. A shortfall is logged.
2. Each peer line this game did not skip is resolved again and added. An
   item without extra data goes in with `AddObjectToContainer`. An item with
   extra data cannot have its stack built here, so each unit is placed as a
   reference (`PlaceObjectAtMe`), given its extra data, and picked up
   (`PickUpObject`, no sound). A player enchantment is rebuilt with
   `BGSCreatedObjectManager::AddWeaponEnchantment` or
   `AddArmorEnchantment` from its effects (magic effect, magnitude, area,
   duration, cost) and attached with its maximum charge; a weapon below it
   gets `ExtraCharge`. A plugin enchantment is attached as it is.
3. A chat notice lists what was given and received.

A pre-enchanted plugin weapon carries no `ExtraEnchantment`, so its charge
is not sent and it arrives full.

## Cancelling

| Event | Reason | Where |
| --- | --- | --- |
| Cancel, Escape, the window's close button | `CANCELLED` | client |
| The decline key, `Decline trade` | `DECLINED` | client (`TradeAnswer`) |
| Combat (`IsInCombat`) or a `TESHitEvent` on the player while a trade is invited or open | `COMBAT` | client |
| Anything taking input from the open window: a game menu, the console, dialogue, a loading screen | `MENU` | client |
| The window cannot take input within 2 s of opening | `FAILED` | client |
| Over `trade.keep_radius` (1000) apart, another space, `Exit` | `OUT_OF_RANGE` | server |
| A disconnect | `DISCONNECTED` | server |
| No answer within `trade.invite_seconds` | `EXPIRED` | server |
| The local connection lost, or trade messages dropped from a full queue | — | client, locally |

A local cancel sends its message and ends the trade at once; the server's
close for it is then ignored. Each end is a chat notice for both players.

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
5. `TESHitEvent` arrives for every hit on the player, on a thread other than
   the main loop.
6. The input contexts that already release a focused window cover every menu
   a script can open, such as message boxes and dialogue started by an NPC.
7. `ExtraTextDisplayData::customNameLength` is the length of the given name
   before the tempering tier in `displayName`.

## Acceptance criteria

1. `Trade` in a player's menu is enabled only for a player standing near;
   the server refuses a far one with a notice.
2. The invited player sees the prompt and a notice without losing control;
   the keys and the menu answer it; it expires after 30 s; the sender is told
   each outcome.
3. Invitations from muted or vanished players never show and expire for the
   sender.
4. Both windows show both offers within a moment of an edit, and an edit
   takes both acceptances back.
5. A completed trade moves exactly the offered items and counts, with their
   enchantments, tempering, names, and souls, both ways, and nothing else.
6. An item, or an enchantment's effect, from a plugin the receiver lacks is
   marked in both windows with that plugin and stays with the giver; the
   rest is exchanged.
7. Cancel, Escape, a disconnect, combat, a hit, any game menu, a loading
   screen, and walking apart each close both windows without changes.
8. Quest items and player-made potions show why they cannot be offered.

## Known limitations

- A trade the server committed reaches a client only while it stays
  connected: one that disconnects in that moment does not apply its side,
  while the other does.
- A player enchantment with more than 4 effects cannot be offered.
- Charge on a pre-enchanted plugin weapon is not kept.
- The client's request radius is the server's default; a server configured
  wider or narrower still decides, and the menu may enable or disable
  `Trade` against it.
- The window does not show weights or values.
