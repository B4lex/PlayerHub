# Off-Hand Attacks, Blocking, and Bashes

## Status

Implemented, pending a Windows build and in-game check. The server side is
covered by unit tests. Graph event names and transitions come from the vanilla
`Skyrim - Animations.bsa` behavior files (`1hm_behavior.hkx`, `0_master.hkx`,
`blockbehavior.hkx`, `bashbehavior.hkx`) and the `Skyrim.esm` idle records. The
items under [Engine assumptions](#engine-assumptions) still need to be checked in
game.

## Goal

A remote actor shows what its source player does with the off hand and with the
block button:

- **Left-hand attacks.** A swing of a left-hand weapon, or a left punch, plays
  the left-hand swing on peers instead of a right-hand one.
- **Dual-wield attacks.** Attacking with both weapons at once plays the
  dual-wield swing.
- **Blocking.** The actor raises its block (shield, weapon, torch, or fists)
  while the source blocks, standing or moving, and lowers it when the source
  does.
- **Bashes.** A bash from a block (shield, one- or two-handed weapon, torch) and
  a bow bash play one bash on peers. A bash from a held block returns to the
  block.

Nothing is hit, staggered, or damaged. Only the animation plays.

## Non-goals

- Power attacks, power bashes, sprint attacks, and their directions. As in
  spec 004, a power attack is presented as the plain attack of the same hand,
  and a power bash as a bash.
- Shield charge (`BlockBashSprint`), block hit reactions, and bash failures.
- Crossbow bashes. The crossbow attack branch is not replicated (spec 011).
- Bow zoom. With a bow, the block button zooms (`BowZoom` sends `blockStart`)
  and is never presented as a block.
- Damage, stamina, perks, and targeting of any kind.

## Vanilla behavior graph

The character graph's `1HM_Behavior` state machine, which also hosts the
two-handed, bow, and unarmed stances, has these relevant transitions. `IsNPC`
is 1 on a remote actor.

| From | Event | Condition | To |
| --- | --- | --- | --- |
| Ready | `blockStart` | `IsNPC == 1` (a second one covers players) | Block |
| Ready | `attackStartLeftHand` | none | Attack |
| Ready | `AttackStartH2HLeft` | left hand empty, right hand not a one-handed weapon | Attack |
| Ready | `attackStartDualWield` | both hand types one-handed weapons (1..4) | Attack |
| Ready | `bashStart` | `IsNPC == 1`, or a bow or crossbow in the right hand | Bash |
| Block | `blockStop` | none (two variants on `iBlockState`) | Ready |
| Block | `bashStart`, `bashPowerStart` | `IsNPC == 1` | Bash |
| Bash | `bashExit` (clip annotation) | `iWantBlock == 1` | Block |
| Bash | `bashExit` | `iWantBlock == 0` | Ready |

`bashStart` plays the whole bash. `BlockBashIntro` and `bashRelease` belong to
the failed-bash branch and are not used. The spell-in-right-hand graph
(`MRh_Shield_OR_Torch_BehaviorGraph`) only enters a bash from its block state,
which is also how the source reached it. Its left weapon graph (`MRh_1HM_Behavior`)
takes `attackStartLeftHand`.

## Why a block is state and a bash is an action

A block lasts as long as the button is held, so it is latest-wins **checkpoint
state**, like a bow draw (spec 011). A late joiner sees an ongoing block, and a
lost lowering cannot leave an actor blocking forever.

A bash and a swing are short and can repeat faster than the checkpoint interval,
so they stay in the ordered **action stream** (spec 004).

## Protocol

### `player:update`

`blocking` is added to the hand presentation fields. A missing field is
`false`, so recordings made before this change still load. A present field must
be a boolean. It is kept in `Multiplayer::HandPresentation`.

### `animation:action`

The `attack` family's `action` vocabulary grows from `normal` to:

| Action | Meaning | Receiver event |
| --- | --- | --- |
| `normal` | A right-hand or unarmed right swing | `attackStart` |
| `left` | A left-hand weapon or unarmed left swing | `attackStartLeftHand`, or `AttackStartH2HLeft` with an empty left hand |
| `dual` | A dual-wield swing | `attackStartDualWield`, falling back to `attackStart` when rejected |
| `bash` | A bash of any weapon, shield, or torch | `bashStart` |

Every action still requires the `weaponDrawn` prerequisite, a contiguous action
sequence, and a compatible checkpoint. Graph event names never cross the
network. The client and the server reject any other action.

## Source capture

### Attacks

`CombatSync::CaptureLocalAttack` still watches the player's
`ATTACK_STATE_ENUM`:

- A melee attack starts on the edges of spec 004. With a bow or crossbow only
  bashes count, since a shot passes through the melee states too.
- A bash starts when the state enters `kBash`, including from an attack.
- A swing that follows a bash (`kBash` to `kDraw` or `kSwing`) starts a new
  attack, as the bash state accepts `attackStart`.

The action is chosen when the attack starts:

1. `kBash`, or attack data with the bash flag, is `bash`.
2. Otherwise the high process's current `BGSAttackData` is the race's entry for
   the graph event that started the attack. An event containing `DualWield` is
   `dual`. One ending in `LeftHand` or `H2HLeft` is `left`. `attackPowerStartLeft`
   is a right-hand power attack toward the left, so only these suffixes count.
3. Otherwise the attack data's left-attack flag (`BGSAttackData::IsLeftAttack`)
   selects `left`, and anything else, including missing attack data, is
   `normal`.

Captured actions keep their order in a bounded queue (32) and are published
against the next checkpoint, as before.

### Blocking

Each pass, `blocking` is `Actor::IsBlocking()` while the weapon is drawn and the
right hand holds no bow or crossbow. `IsBlocking()` is false while a bash plays,
even from a held block (observed in game), which made peers lower the block
and bash into the ready state. During `kBash` the player's `iWantBlock` graph
variable, which the vanilla `bashExit` transition uses to return to the block,
decides instead, or `ActorState::wantBlocking` when the graph has no such
variable. Like the other hand presentation fields, it
is ORed across passes until the next checkpoint, and turning on flushes
equipment first.

## Receiver

### Attacks

`ActorAnimation::CanDispatchAttack`, `CanChainAttack`, and `DispatchAttack` take
the action:

- The readiness conditions of spec 004 apply to every action. A bow draw or cast
  (spec 011) still holds every action.
- A held or lowering block holds every action except `bash`. The source can only
  swing after lowering its block, so the swing waits for the block to be lowered
  on the actor too.
- `normal`, `left`, and `dual` chain in the playing attack's combo window. A
  `bash` never chains and waits for the playing attack to finish.
- The chosen event is sent through the firewall bypass with `kAttackingDisabled`
  cleared for the synchronous call. A rejected `dual` is retried as `attackStart`
  in the same call and logged.
- Ownership, retries, timeouts, and the locomotion replay afterwards are those
  of a normal attack, except that a bash also ends on its `bashExit` annotation.
  A remote actor's attack state does not report bashes (observed in game), so
  otherwise every bash held the actor for the 1.25 s start fallback. Exits
  observed before a bash is dispatched are discarded.

### Blocking

`CombatSync::SyncHandPresentation` calls `ActorAnimation::ApplyBlock` before the
bow draw and casts. The desired state is the checkpoint's `blocking` while it is
drawn, grounded, and not in furniture. A new block also needs settled hands and
no playing attack.

- **Raise.** Requires the conditions of a bow draw (spec 011) and no bow draw or
  cast. `SetBlockRequest(true)` sets `ActorState::wantBlocking` and the graph's
  `iWantBlock`, then `blockStart` is sent through the bypass like a bow event.
  A rejected start clears the request and retries every 0.1 s.
- **Hold.** The request is set again on every pass, so a bash's `bashExit`
  returns to the block state.
- **Lower.** The request is cleared and `blockStop` is sent. A playing bash
  rejects it and then exits to the ready state by itself. A block state that
  rejected it is asked again every 0.1 s while no attack plays. The block is
  lowered once `IsBlocking` is false, or on the accepted event when the graph has
  no such variable. After 1.5 s, `blockStopInstant` drops it. Locomotion is then
  replayed as after an attack.
- A graph reset clears all block state. The next pass clears a request left on
  the actor.
- A block counts as hand presentation. `AnimationSync` therefore holds the
  actor's `weaponDrawn` and `jumping` while it is held or lowering, furniture
  entry waits for it, and a bow draw or cast does not start.

### Moving lower body

The lower bodies of the block (`Blocking_LowerBody_Behavior`), bow draw
(`Bow_Attack_LowerBodyBehavior`), and cast (`DefaultMagicCastingBehavior`) state
machines choose their standing or moving start state from
`iSyncIdleLocomotion` when they are entered. The engine leaves it at 0 on a
remote actor (the weapon replay diagnostics log it as 0 while running), so a
block, bow draw, or cast started while moving would enter the standing pose
while network translation continues. After such a start,
`ActorAnimation::AdvancePresentationLocomotion` waits for the graph to enter the
branch (two synchronization passes, and for a cast also its `IsCastingLeft` or
`IsCastingRight` variable, for at most 0.5 s) and then sends `moveStart`, the
lower body's standing-to-moving edge. It is sent once: a rejection means the
lower body is not standing, and retries were never accepted in game.
Nothing is sent while standing, while a bash owns the graph (its locomotion
replay sends `moveStart`), or after the presentation has ended. Movement that
starts later during a presentation reaches the lower body through the ordinary
locomotion events.

## Isolation

No new hooks. A bash reaches the melee hit frame (`WeaponSwingHitHook`, spec 008)
like a swing, and blocking only changes the actor's own presentation. Hits that
local actors land on a blocking remote actor stay harmless, and the hit
reactions the graph would play are blocked by the animation firewall.

## Engine assumptions

To verify in game:

1. `high->attackData` holds the starting attack's data when the attack state
   first changes. The log line "observed local attack state edge" shows the
   chosen action for each attack. Check left-hand, dual-wield, unarmed left, and
   sprint left attacks.
2. The player's attack state enters `kBash` for shield, weapon, torch, and bow
   bashes, including power bashes.
3. `Actor::IsBlocking()` is true for the whole block hold and false otherwise,
   and stays false during a bow zoom.
4. A remote actor accepts `blockStart` from its ready state for shields,
   one-handed weapons, two-handed weapons, torches, and fists, and the block pose
   follows walking and running.
5. The graph variable `IsBlocking` follows the block state on a remote actor. If
   it does not, a lowering block completes on the accepted `blockStop`.
6. `bashStart` from a raised block plays the bash, and with `wantBlocking` and
   `iWantBlock` set, `bashExit` returns to the block. A bow bash plays from the
   ready state.
7. `attackStartLeftHand`, `AttackStartH2HLeft`, and `attackStartDualWield` are
   accepted by a remote actor holding the matching hands, including a left weapon
   with a spell in the right hand, and they chain in the combo window.
8. The graph output sink receives `bashExit` for every remote bash ("Observed
   actor bash exit marker"). Without it, the 1.25 s start fallback ends a bash.
9. The player's `iWantBlock` stays 1 during a bash from a held block and is 0
   during one whose block button was released.
10. A bash's hit frame goes through the hooked melee hit and hits nothing.

## Acceptance criteria

1. Left-hand weapon swings, unarmed left punches, and dual-wield swings play the
   same hand on peers, standing or moving, in the same order as the source's
   attacks. Power attacks play the plain attack of the same hand.
2. Blocking with a shield, a one- or two-handed weapon, a torch, or fists raises
   the block on peers for as long as the source holds it, standing or moving. A
   bow zoom is not shown as a block.
3. A bash from a held block plays one bash on peers and returns to the block
   while the source still holds it. A bow bash plays one bow bash.
4. A swing right after lowering the block plays after the block is lowered.
   Sheathing, jumping, or sitting lowers the block first. A late joiner sees an
   ongoing block.
5. Nothing is hit, staggered, or damaged by a remote bash or swing.
6. Recordings made before this change still replay. A non-boolean `blocking` or
   an action outside the vocabulary is rejected by the server and the client.
