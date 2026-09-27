# Bow Draw and Spell Casting Presentation

## Status

Implemented, pending a Windows build and in-game check. The server side is
covered by unit tests. Graph event names come from the vanilla
`Skyrim - Animations.bsa` behavior files and the `Skyrim.esm` idle records. The
items under [Engine assumptions](#engine-assumptions) still need to be checked in
game.

## Goal

A remote actor shows when its source player draws a bow or casts a spell:

- **Bows.** The actor draws, holds the draw as long as the source holds it, and
  releases. Its quiver shows the source's arrows, and it nocks one when it draws.
- **Spells.** A hand holding any spell or scroll holds one placeholder spell,
  Healing Hands (`Skyrim.esm` `0x04D3F2`, a concentration spell aimed at a
  target actor). Its hand effect shows in that hand while the weapon is drawn.
  While the source casts from that hand, the actor casts Healing Hands from
  the same hand for as long as the source casts, whatever the source's spell
  is.

No arrow, projectile, spell effect, or anything else is created. Only the
animation and the hand effect of the equipped spell appear.

## Non-goals

- The source's actual spell, its art, sound, casting type, delivery, or dual
  casting. A dual cast plays the one-handed animation in each hand.
- Casting from staves. A staff stays a weapon (spec 007) and plays no cast.
- Crossbows. `crossbowAttackStart` is a different branch and is not replicated.
- Shouts, powers, bashes, and power-attack or zoom variants of a bow draw.
- Distinguishing a cancelled draw from a release. Peers see a release.
- Damage, magicka, ammo consumption, and targeting of any kind.

## Why state rather than actions

Normal attacks are an ordered action stream (spec 004) because a latest-wins
field loses repeated short swings. A bow draw and a spell cast are the opposite:
what matters is how long they last, and each one lasts much longer than the
0.05 s checkpoint interval. So they are latest-wins **checkpoint state**, like
`weaponDrawn`. A late joiner sees an ongoing draw, and a lost release cannot
leave an actor drawing forever.

## Protocol

### `player:update`

Three booleans are added to the checkpoint:

| Field | Meaning |
| --- | --- |
| `bowDrawn` | The source is drawing or holding a bow draw. |
| `castingLeft` | The source is casting from a spell or scroll in its left hand. |
| `castingRight` | The same for the right hand. |

The server and client treat a missing field as `false`, so recordings made
before this change still load. A present field must be a boolean. The server
always relays all three. They are kept in `Network::PlayerTransform::handPresentation`
(`Multiplayer::HandPresentation`), not in `AnimationState`, because a change to
`AnimationState` restages locomotion. Spec 012 adds `blocking` to the same
structure.

### `player:equipment`

- A hand can be `{"kind": "spell"}` in either hand. It has no `plugin`, `formId`,
  or `weaponType`: receivers always use the placeholder, so the source's spell is
  never sent. A two-handed weapon in the right hand still requires an empty
  left hand.
- A new top-level `ammo` is the equipped arrows' or bolts' `{plugin, formId}`,
  or `null`. A missing `ammo` is `null`, so older recordings still load.

## Source capture

Each pass, before equipment capture, `CombatSync::CaptureLocalHandPresentation`
reads the player. Everything is `false` unless the weapon is drawn.

- `bowDrawn`: the right hand holds a bow and `ATTACK_STATE_ENUM` is
  `kBowDraw`, `kBowAttached`, or `kBowDrawn`. `kBowReleasing` and later states
  are already a release.
- `castingLeft`/`castingRight`: that hand holds a spell or scroll and its
  `MagicCaster::state` is anything from the cast request through release
  (`kUnk01` to `kUnk07`). `kNone` is a readied hand that is not casting.

When any field turns on, equipment is flushed first, as for an attack, so a
new spell or bow reaches peers before its use.

Fields are ORed across passes and sampled by `CheckpointPublisher` for each
checkpoint, then cleared. A presentation shorter than the checkpoint interval
still reaches peers once.

Equipment capture reports a hand holding a `SpellItem` or `ScrollItem` as a
spell hand. `ammo` is the worn `TESAmmo` inventory entry: `Actor::GetCurrentAmmo`
returns nothing for the player, so receivers use the same worn-entry check.

Normal-attack capture (spec 004) ignores attack states while the right hand holds
a bow or crossbow. A shot passes through `kNextAttack` and `kFollowThrough`,
which would otherwise also publish every shot as `attack/normal`.

## Receiver

### Equipment

- A spell hand resolves to the placeholder spell and is equipped with
  `ActorEquipManager::EquipSpell` in that hand's slot. It is not an inventory
  item, so it never counts toward required inventory.
- A placeholder in a hand that should be empty is removed with
  `ActorEquipManager::UnequipSpell` (ID 38902, the target of Papyrus
  `Actor.UnequipSpell`) in that hand's slot. Like `EquipSpell`, it runs
  `ActionForceEquip`, which moves a drawn hand out of its spell pose.
  `Actor::DeselectSpell` sends no graph event and left the hand raised; it
  remains the fallback on runtimes other than AE. A weapon or torch that
  should replace the placeholder is equipped over it.
- Accepted `ammo` goes through the same resolution filter as other items
  (`TESAmmo`). Twenty are kept in inventory and equipped, which fills the
  quiver. Remote shots never spend them.
- `handsMatch` now also requires the equipped ammo to match. That way a bow
  draw, which waits for settled hands, nocks the right arrow.
- Hand and ammo swaps are deferred while a bow draw, its shot, or a cast is
  playing, with the existing 2 s bound.

### Presentation stage

`CombatSync::SyncHandPresentation` runs after `SyncCombat` and is skipped while
furniture owns the actor. For the bow and for each hand, the desired state is
the checkpoint field. It counts only while the checkpoint is drawn, grounded,
and not in furniture. A new draw or cast also needs settled hands and no melee
attack playing. The actor graph operations live in `ActorAnimation`:

- **Start** requires a stable graph, the same conditions as a normal attack
  (settled revision, no weapon transition, recovery, attack, locomotion
  reassert, or bow shot), a fully drawn `WEAPON_STATE`, and the right object in
  the hand (a bow, or a spell in that hand).
- **Bow.** `bowAttackStart` begins the draw, which is held for as long as the
  desired state stays on. A held draw is never dropped without an exit event.
  When the desired state clears, `attackRelease` is sent. An early release is
  rejected until the arrow is drawn, so it retries. After 1.5 s of rejections,
  `attackStop`, then `bowReset`, cancels instead. The shot then owns the graph
  for 0.5 s. If the actor's attack state still reports a bow attack, it waits for
  that too, for at most 2 s. After that, locomotion is reasserted as after a
  normal attack. A remote actor's `ATTACK_STATE_ENUM` does not follow its graph
  (observed in game), so nothing depends on it entering the bow states.
- **Cast.** The hand's own `ActorMagicCaster` casts the placeholder, as NPC
  casting does: `SetCurrentSpell` if needed, then `RequestCastImpl`. Sending the
  graph's cast events directly does not work: the magic graph emits
  `InterruptCast` for a cast its caster did not request, and rejects a directly
  sent `M?h_SpellAimedConcentrationStart` (observed in game). `RequestCastImpl`
  alone only sends `BeginCast*`, and the caster stayed in `kNone` (observed in
  game). So a start also moves an idle caster to `kUnk01`, as the engine's cast
  request does, and sets `bWantCastLeft`/`bWantCastRight`, which the magic graph
  needs to stay in its casting state. The variable is set again on each pass
  while the cast is held. The engine then runs the caster's charge, ready, and
  cast steps. `StartCastImpl` performs the hand's release action
  (`ActionLeftRelease`/`ActionRightRelease`), which selects the cast clip
  through `IDLE` conditions on the spell's casting type and delivery. The human
  aimed branches (`RightCastAimed`, `LeftConcentrationAimed`) take delivery
  `>= 2`, so Healing Hands' target-actor delivery (3) plays the aimed
  concentration clip. The caster then waits in state 4, because its spell fire
  is suppressed (see Isolation).
- **Cast stop.** The variable is cleared and `InterruptCast(false)` is called,
  which performs the hand's interrupt action and then finishes the cast.
  `FinishCast` alone returns the caster to `kNone` but leaves the concentration
  clip looping (observed in game). The stop completes once the caster is `kNone`
  and the hand's `IsCastingLeft`/`IsCastingRight` graph variable is false, or
  after 1 s. No new cast on that hand starts before then. A cast also stops
  when the hand loses its spell. Once no hand is still casting or stopping and
  no other presentation is playing, locomotion is reasserted as after a normal
  attack, because the cast's lower body exits into its standing state (see
  spec 012 under "Moving lower body").
- While a cast is held or stopping, the actor's registry entry marks cast
  ownership. The firewall then passes cast events (`MLh_*`, `MRh_*`,
  `DualMagic_*`, and names containing `Cast`), which the caster sends then and
  on later updates. Names are matched ignoring case: the left release idle
  sends `MLH_SpellRelease_event`. `ApplyRemoteReplicaPolicy` also leaves
  `kCastingDisabled` cleared.
- Bow events are sent through the firewall bypass. `kAttackingDisabled` and
  `kCastingDisabled` are cleared only for that synchronous call, like
  `attackStart`. Starts and rejected events retry at most every 0.1 s.
- A draw or cast started while moving selects its moving lower body with
  `moveStart`, as described in spec 012 under "Moving lower body".
- All presentation state is cleared whenever the graph is invalidated
  (recovery, 3D or graph loss, graph replacement).
- Readied spell hands draw through the magic equip branch. The firewall allows
  `Magic_Equip`, `MagicForceEquip`, and `MagicWeap_ForceEquip`, and the
  `ActionForceEquip` events that swap a single drawn hand to or from a spell:
  `MagicForceEquipRight`, `MagicForceEquipLeft`, `WeapSoloEquip`, and
  `Magic_Solo_Equip`. Its
  `Magic_Equip_Out[Moving]` and `Magic_Unequip_Out[Moving]` annotations complete
  a weapon transition like the weapon equip exits. Without them, the draw never
  reached `kDrawn` and no cast could start.

### Interaction with other features

- While a presentation is playing, `AnimationSync` holds the actor's current
  `weaponDrawn` and `jumping`, and locomotion keeps following the checkpoint. A
  sheathe or jump in the same checkpoint therefore first ends the presentation
  (whose desired state is already off), then applies.
- A normal attack cannot start while a presentation is playing.
- Furniture entry waits for it, like an attack.

## Isolation

The draw and cast animations natively run the release paths. These hooks keep a
registered remote actor from releasing anything. Other actors pass through
unchanged. The vtable hooks install on every runtime through the address
library. The spell fire entry detour, like the combat isolation detours of
spec 008, is verified against 1.7.104 only.

| Hook | Effect for a remote actor |
| --- | --- |
| `ArrowReleaseHandler::ExecuteHandler` (the `arrowRelease` annotation) | Returns handled without launching the arrow or spending ammo |
| Spell fire (ID 34407 on 1.7.104, entry detour), reached from the `MLh_SpellFire_Event` / `MRh_SpellFire_Event` annotations and from a casting caster's update | Returns true without spending the cost, applying or launching the spell, or entering the caster's casting state (6) |
| `ActorMagicCaster::CastSpellImmediate` (vfunc 1) | Does not cast |
| `ActorMagicCaster::CheckCast` (vfunc 0xA) | Returns true with `kOK`, so magicka or the casting-disabled flag never interrupt the animation |

The projectile-hit hook from spec 008 still drops any hit a remote actor's
projectile would land.

## Engine assumptions

To verify in game:

1. `MagicCaster::state` is `kNone` while a spell hand is readied but not casting,
   and stays in `kUnk01`..`kUnk07` for the whole button hold of both concentration
   and fire-and-forget spells. The log line "observed local hand presentation"
   shows each change.
2. `ActorEquipManager::EquipSpell` equips a spell on an actor that does not know
   it, and `GetEquippedObject` then returns it (confirmed in game with Sparks).
   A drawn actor shows the spell's hand effect. `ActorEquipManager::UnequipSpell`
   empties only the given hand and lowers it through `ActionForceEquip`
   (`WeapOutRightReplaceForceEquip` for the right hand, `WeapSoloEquip` or
   `Magic_Solo_Equip` for the left).
3. `DrawWeaponMagicHands(true)` readies spell hands with no graph events other than
   the allowed magic equip events. Check `latest.log` for other blocked `Magic*`
   or `M?h_*` events during a draw.
4. `kUnk01` plus `RequestCastImpl` and a set `bWantCast*` variable start the cast
   animation on a remote actor, and the concentration clip loops until
   `InterruptCast` (confirmed in game with Healing Hands in the right hand).
   Check the left hand, dual casting, and casting while walking or running.
5. Spell fire is the only path that releases a remote actor's animated cast.
   Hooking the `M?h_SpellFire_Event` handlers' `ExecuteHandler` did not stop the
   caster from reaching state 6 (observed in game), so spell fire itself is
   hooked, and it suppressed every fire (confirmed in game). Its other callers
   appear to be casts without an animation, and `CastSpellImmediate` covers the
   scripted path. If a beam or heal still appears, extend suppression to
   `MagicCaster::FindTargets`.
6. The character graph accepts `bowAttackStart` from a drawn bow and holds the
   drawn pose (confirmed in game). It also takes `attackRelease` and accepts
   locomotion events during the draw, and the release clip ends within 0.5 s.
7. `ArrowReleaseHandler` is the only path that launches the arrow. With it
   suppressed, the nocked arrow model is still removed by the release clip's own
   annotations.
8. Equipping twenty arrows with `EquipObject` shows them in the quiver.

## Acceptance criteria

1. A source holding a bow with arrows shows the same bow and arrows, in the
   quiver, on peers.
2. Drawing, holding, and releasing a bow plays the same draw, hold duration, and
   release on the remote actor, standing or moving. No arrow is fired or spent,
   and nothing is hit.
3. A source holding any spell or scroll in a hand shows Healing Hands' hand effect in
   that hand on peers while drawn. An empty or weapon hand shows no spell.
4. Casting from a hand plays one Healing Hands cast in that hand on peers
   for as long as the source casts. No projectile, beam, effect, or sound of a
   released spell appears, and nothing is affected.
5. Sheathing, jumping, or sitting ends a draw or cast before the stance changes.
   A late joiner sees an ongoing draw or cast.
6. Recordings made before this change still replay. Malformed hand presentation
   or ammo payloads are rejected by the server and the client.
