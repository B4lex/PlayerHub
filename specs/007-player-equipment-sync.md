# Player Equipment Synchronization

## Status

Implemented, but not yet built or verified in game. The server side is covered
by unit tests. The items under [Engine assumptions](#engine-assumptions) still
need to be checked in game.

## Goal

A remote actor wears and wields what the source player has equipped: armor,
clothing, jewelry, shield, the weapons in either hand, and a torch. When the
source changes equipment, the actor follows within a fraction of a second.
There is no respawn and no equip animation.

The actor's inventory holds **only** the synchronized equipped items. The rest
of the source's inventory is never sent, and the actor has nothing beyond that
set, so it cannot equip anything else.

Before this change actors had no synchronized equipment. `weaponDrawn`
(spec 004) always drew fists.

## Non-goals

- The unequipped inventory, gold, item counts beyond what equipping needs, and
  carry weight.
- The equipped shout or power. Ammo and spell or scroll hands were added by
  spec 011: a spell hand is sent as `{"kind": "spell"}` and shown as a
  placeholder spell, and `ammo` carries the equipped arrows.
- Enchantments, tempering, poison, charge, and player-given names. Enchanted
  items are reduced to their unenchanted base item (see
  [Enchanted and crafted items](#enchanted-and-crafted-items)).
- Items whose defining plugin the receiver does not have. They are dropped
  here. Spec 019 replaces them with configured fallback items, using the hints
  the protocol already carries.
- Equipment while transformed (werewolf, vampire lord). As in spec 006, peers
  keep the last equipment captured in a playable race.
- Equip and unequip sounds and animations on the actor.
- RaceMenu/NiOverride armor overlays, and skeleton-dependent physics (SMP/CBPC).

## Protocol

Equipment is reconstructable, latest-wins state. It changes more often than
appearance but far less often than the 20 Hz checkpoint, so it gets its own
event. The client emits `player:equipment`:

```json
{
  "revision": 7,
  "armor": [
    { "plugin": "Skyrim.esm", "formId": 77385, "slotMask": 4 },
    { "plugin": "Skyrim.esm", "formId": 77389, "slotMask": 512 },
    { "plugin": "Dawnguard.esm", "formId": 5095, "slotMask": 1 }
  ],
  "rightHand": { "plugin": "Skyrim.esm", "formId": 77936, "kind": "weapon", "weaponType": 1 },
  "leftHand": { "plugin": "Skyrim.esm", "formId": 110, "kind": "torch" }
}
```

### Form references

Forms use the load-order independent `{plugin, formId}` reference defined in
spec 006: `plugin` is the file that defines the form, and `formId` is the
file-local ID. Dynamic (`FF`) forms cannot be referenced. If an item cannot be
reduced to a referenceable base item, the entry is omitted.

The **sender publishes every referenceable equipped item, vanilla or not**. All
filtering happens on the receiver, which resolves an item from any plugin it has
loaded. Peers that share a mod see its items; a peer missing the plugin drops
the entry, or shows a fallback for it (spec 019).

### Fields

| Field | Meaning | Validation |
| --- | --- | --- |
| `revision` | Source-local monotonic counter | integer `1..2^63-1`, strictly increasing per connection |
| `armor` | Worn playable `TESObjectARMO`s, including shields | at most 32 entries; form references unique |
| `armor[].slotMask` | `BGSBipedObjectForm` slot mask, bit 0 = slot 30, limited to the synced slots | non-zero; only bits of the synced slots |
| `rightHand` | Right-hand object, or `null` | hand entry |
| `leftHand` | Left-hand object, or `null` | hand entry |
| `hand.kind` | `weapon` or `torch` | enum; `torch` only in `leftHand` |
| `hand.weaponType` | `WEAPON_TYPE` `1..9`; present only for `weapon` | two-handed types (`5` 2H sword, `6` 2H axe, `7` bow, `9` crossbow) only in `rightHand`, and then `leftHand` must be `null` |

Plugin names are 1..260 characters and `formId` is `0..0xFFFFFF`, as in
spec 006. The hint fields `slotMask` and `weaponType` do not
affect resolution. They select the fallback of spec 019. A receiver
still validates them, but the locally resolved form is authoritative for how an
item is equipped.

A hand holding nothing or bare fists (`WEAPON_TYPE` `0`) is `null`. Spell hands
and `ammo` are specified in spec 011.
Dual-wielding the same one-handed weapon sends the same reference in both hands.

### Server

Same lifecycle as `player:appearance`:

- Validates `player:equipment`. Malformed payloads and non-increasing revisions
  are rejected with an acknowledgement error.
- Stores the latest equipment per connection and relays it to every other client
  as `player:equipment` with `playerId`.
- On `connect`, sends every stored equipment of the other connections to the new
  client.
- Forgets the equipment on disconnect.
- When recording is enabled, appends `player:equipment` to the recording. Replay
  accepts the event.

## Source capture

### Triggers

- A `TESEquipEvent` sink marks equipment dirty when the actor is the player. The
  sink runs on whichever thread fires the event, so it only sets an atomic flag.
- The main-loop pass captures once the dirty flag has been quiet for 100 ms.
  This coalesces multi-event changes: a favorites swap (unequip then equip), a
  full outfit change, or a two-handed weapon clearing both hands.
- A full capture also runs once per second without a dirty flag, as a safety net
  for changes that fire no event (game load, script equips, race change).
- On the frame a local attack is observed, equipment is captured and, if it
  changed, published immediately, bypassing both the quiet window and the
  minimum publish interval. The attack action is only emitted with a later
  checkpoint, so the receiver can apply the new weapon before it plays the
  attack.

### Capture

On the main-loop thread, from the player:

- Skip capture if the actor's race is not playable (transformation), as in
  spec 006.
- `armor`: every worn inventory entry (`ExtraWorn` or `ExtraWornLeft`) whose
  object is a `TESObjectARMO` without the non-playable flag. Skin armor never
  appears in inventory. Deduplicate by reference after enchantment reduction.
  Only the synced slots count: Head (30), Hair (31), Body (32), Hands (33),
  Amulet (35), Feet (37), Shield (39), and Circlet (42). `slotMask` keeps only
  those bits, and armor covering none of them is not sent.
- `rightHand` and `leftHand`: `Actor::GetEquippedObject(false/true)`.
  - `TESObjectWEAP` with a type other than hand-to-hand → `weapon` with its
    `weaponType`.
  - `TESObjectLIGH` that can be carried, in the left hand → `torch`.
  - A shield is not a hand entry. It is worn armor and appears in `armor`.
  - A two-handed weapon, bow, or crossbow is reported in `rightHand` only, even
    if the engine also reports it for the left hand.
  - A spell or scroll → `spell` (spec 011).
  - Anything else → `null`.

### Enchanted and crafted items

A player-enchanted item keeps its original base form and carries the enchantment
as extra data (`ExtraEnchantment`). Only the form is sent, so the enchantment is
dropped without any special handling. Vanilla pre-enchanted records (for
example *Iron Sword of Embers*) are separate forms that point at their
unenchanted item through `templateWeapon` or `templateArmor`.

The sender follows that template chain, bounded to 8 steps, to the first form
with no template, and publishes that form. If it is dynamic, the item is
skipped. The remote actor therefore shows the unenchanted base model with no
enchantment shader. Player-crafted, non-enchanted items are ordinary plugin
forms and are sent as is.

### Publishing policy

- Publish when the capture differs from the last published equipment. The
  revision is excluded from the comparison, and the armor list is compared as a
  set.
- Publishes are at least 250 ms apart. A capture that becomes publishable inside
  that interval is sent when the interval ends, and only the newest capture is
  sent.
- Each publish increments `revision`.
- On every transition to connected, the last equipment is re-published.

## Receiver

### Accepting updates

The network thread keeps only the newest pending equipment per player (by
revision) and discards it on `player:disconnect`. The main-loop pass stores it on the
remote player state, independent of whether a actor reference currently
exists. A rejected payload is logged with the name of the offending field.

### Resolution filter

An entry is accepted if the form resolves locally through
`TESDataHandler::LookupForm` and has the expected type: `TESObjectARMO` for
`armor`, `TESObjectWEAP` for `weapon`, or a carriable `TESObjectLIGH` for
`torch`. The defining plugin is not restricted: an item from any plugin the
receiver has loaded resolves, at whatever load-order index it sits locally.

Everything else is dropped from the resolved set. Each drop is logged once per
revision with player, plugin, form ID, and reason (`missing`, `wrong-type`). A
reference names the plugin that *defines* the form, so a form is still accepted
when a local mod overrides its record, and the receiver sees its own version of
that item.

The filter is a single policy function that takes an entry and returns a
resolved form or a drop reason. The fallback of spec 019 replaces the drop
branch without touching capture, protocol, or reconciliation.

Consequences in this phase, all accepted:

- A dropped body armor leaves the actor in the race's naked skin.
- A dropped weapon leaves the hand empty. The actor draws fists and plays the
  unarmed attack.

### Actor inventory ownership

The actor's inventory must always equal the resolved equipped set. To stop
anything else from entering it:

- The runtime actor base from spec 006 clears its copied `TESContainer` entries
  and sets `defaultOutfit` and `sleepOutfit` to `nullptr`. The local player base
  is never modified.
- The fallback actor built from the local player base, and the construction
  template (`00013BBF`), may still put initial items into the reference. So
  reconciliation, not the base, is the authority.

### Reconciliation

This runs in the main-loop pass for a actor whose actor is ready and 3D-loaded.
Each check measures the live inventory counts, worn armor, and equipped hand
objects against the resolved set. Before equipment is received, the resolved
set is empty, so construction-template items are still removed. When the live
state differs, a mutation pass runs:

1. Compute required counts: one per accepted armor entry, and for the hands one
   per hand object, so dual-wielding the same weapon requires two.
2. Remove from the actor's inventory every object not required, and the excess
   count of required ones. That includes gold and items added by leveled lists,
   scripts, or death items. Removed items are deleted, never transferred to any
   container or to the player.
3. Unequip any currently equipped armor or hand object that is not in the
   resolved set.
4. Add missing required objects.
5. Equip armor first, then the right hand, then the left hand, with
   `ActorEquipManager::EquipObject` using the matching `BGSEquipSlot`
   (right/left, or either for two-handed), `queueEquip=false`,
   `forceEquip=true`, and `playSounds=false`.
A mismatch is checked every frame until the first mutation pass, so a new
revision applies immediately. After a pass, and while the state matches, the
check runs every 0.25 s. At most 4 mutation passes run before the live state
matches. After that the actor keeps whatever it reached until the next
revision or respawn. Periodic checks continue, and a later match resets the
budget.

The actor must not change its own equipment. AI auto-equip of "best" armor or
weapon, outfit re-application on cell load, and unequip-on-hit are either
blocked for actors or undone by the next reconciliation. Reconciliation
compares against the live equipped state rather than only the last applied
revision.

### Interaction with weapon and attack sync (spec 004)

- **Spawn and respawn.** Equipment is reconciled before the first weapon-stance
  reconstruction. A actor whose latest checkpoint is `weaponDrawn=true` waits
  for hand reconciliation, bounded to 1 second, before its native draw request.
  That way it does not draw fists and then pop a weapon in.
- **Armor** changes apply immediately in every graph state.
- **Hand** changes are instant swaps on the live actor, including while drawn.
  They are deferred while a native draw/sheathe transition or a synchronized
  attack owns the graph, and applied as soon as it releases. The deferral is
  bounded to 2 seconds, after which the swap is applied anyway.
- **Attacks.** An attack action becomes dispatchable only after the latest
  received equipment revision has been reconciled for the hands. Combined with
  the sender's pre-attack flush, this plays the attack with the correct weapon.

### Respawns

Retiring a actor (coordinate-space change, appearance revision) keeps the
remote equipment state. The new reference is reconciled from it after spawn.
Equipment changes never trigger a respawn.

## Implementation notes

- `FormReference.h` holds the shared plain form reference and its validator.
  `FormLookup.{h,cpp}` encodes and resolves it for both appearance and
  equipment.
- `EquipmentState.h` holds plain data plus `IsValidEquipment`, shared by capture
  and the network parser. `PlayerEquipment.{h,cpp}` holds capture, the
  resolution filter, receive state, and reconciliation, mirroring the appearance
  split.
- `FrameUpdate.cpp` calls reconciliation before animation state is advanced.
  It holds a sheathed-to-drawn step in `GetPresentableAnimationState` and gates
  attack dispatch on `AreHandsSettled`.
- The server adds `parse_player_equipment` next to `parse_player_appearance` and
  reuses its form-reference validation.

## Engine assumptions

To verify in game:

1. `TESEquipEvent` fires for player favorites swaps, the inventory menu,
   hotkeys, and script `EquipItem`/`UnequipItem`. Where it does not, the 1 s
   poll must cover it.
2. `GetEquippedObject(true)` reports two-handed weapons, bows, and crossbows in
   the left hand as well. The capture rule above handles either answer.
3. `EquipObject` with `forceEquip=true` on a actor with a drawn weapon swaps the
   weapon in place. It must update the graph weapon-type variables (for example
   `iRightHandType`, `iLeftHandType`) so the next attack uses the new weapon's
   animation, without events the actor firewall blocks. If it does not, hand
   swaps while drawn fall back to the existing instant sheathe/draw path.
4. Actors do not auto-equip or unequip on actor process updates once their
   inventory holds only the synchronized set and their outfits are cleared.
5. Removing items from a actor reference with no destination deletes them and
   fires no player-facing notification.
6. The template chain of every vanilla enchanted weapon and armor ends at a
   non-dynamic, non-enchanted form. Player enchanting keeps the base form and
   only adds `ExtraEnchantment`.
7. `ActorEquipManager::EquipObject` with `queueEquip=false` updates
   `ExtraWorn` and the process's equipped hand objects before the next check,
   0.25 s later.

## Acceptance criteria

1. A remote actor shows the source's armor, clothing, jewelry, shield, weapons
   in both hands, and torch, from any plugin the receiver also has.
2. The actor's inventory holds exactly those items and nothing else, including
   after cell reload, respawn, and a hit reaction.
3. Changing equipment on the source (menu, favorites, hotkey) updates the actor
   within 0.5 s with no respawn, equip animation, or sound.
4. Late joiners see the current equipment without the source changing anything.
5. Items whose defining plugin the receiver does not have are dropped with a log
   entry. A receiver with the same mod shows them.
6. Player-enchanted items appear as their unenchanted base item.
7. Drawing a weapon right after spawn or a swap draws the synchronized weapon,
   and the next attack uses its animation.
8. The local player's inventory and equipment never change because of a actor.
9. Malformed or stale equipment payloads are rejected by the server and the
   client.
