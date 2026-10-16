# Combat Presentation

## Status

- **Weapon stance and normal attacks:** implemented and run in game on Skyrim
  AE 1.7.104.
- **Bow draws and spell casting:** implemented; a Windows build and a full
  in-game check are pending. Confirmed in game: `EquipSpell` on an actor that
  does not know the spell, the Healing Hands right-hand cast looping until
  `InterruptCast`, a held bow draw, spell fire suppression, and the last cast
  stop waiting for the graph to leave the cast branch before locomotion is
  replayed (a replay with running casts, 2026-10-14). The rest is under
  [Engine assumptions](#engine-assumptions).
- **Spell look** (2026-10-15): the hand look's source and the form copies were
  read from the 1.7.104 executable; server unit tests and a Linux syntax check
  only; not built on Windows or run in game, and not checked on SE 1.5.97.
- **Left-hand and dual attacks, blocking, and bashes:** implemented; Windows
  build and in-game check pending.
- **Chain capture from `kHit` and `kDraw`, and single dual-wield bashes**
  (2026-10-16): a peer with an attack replacer published one attack per chain
  and two per bash (`latest.log`, consecutive `actionSequence` values); the
  attack state writers were read from the 1.7.104 executable and the vanilla
  clip annotations from `animationdatasinglefile.txt`. Linux syntax check
  only; not built on Windows or run in game.
- **Attack catch-up** (2026-10-14): the weapon speed bindings were read from
  the vanilla behavior files and the weapon speed channel layout from the
  1.7.104 executable; Linux syntax check only; not built on Windows or run in
  game; the channel layout on SE 1.5.97 is not checked.
- **Power attacks:** implemented; Windows build and in-game check pending,
  except the observations noted below.

Graph event names, transitions, and conditions come from the vanilla
`Skyrim - Animations.bsa` behavior files (`0_master.hkx`, `1hm_behavior.hkx`,
`blockbehavior.hkx`, `bashbehavior.hkx`) and the `Skyrim.esm` idle and race
records. The server side is covered by unit tests.

## Scope

A remote actor shows its source's weapon stance, melee and bow attacks,
bashes, power attacks, blocking, bow draws, and spell casts, standing or
moving. Only animations and hand effects play. Nothing is hit, staggered,
damaged, spent, or targeted, and no arrow, projectile, or spell effect is
created.

The wire fields are in [Network](network.md#checkpoint-and-context) and
[Network](network.md#actions); the synchronized weapons, spell hands, and
ammo are in [Appearance and equipment](appearance-and-equipment.md#equipment);
isolation of hits is in [Remote actor](remote-actor.md#combat-isolation).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/combat/CombatSync` | Source attack and hand-presentation capture; receiver action queue, dispatch gating, and hand presentation stage |
| `sync/animation/AnimationSync` | Weapon stance as part of the presentable state; graph epoch |
| `actor/ActorAnimation` | `CanDispatchAttack`, `CanChainAttack`, `CanPowerUpAttack`, `DispatchAttack`, `ApplyBlock`, bow and cast operations |
| `sync/equipment/SpellStandIn` | Spell hand look capture, resolution, and the stand-in spells |
| `actor/AnimationEvents.h` | Event names and `GetPowerAttackEventDirection` |

## State or action

- **Checkpoint state** for what lasts: `weaponDrawn`, `bowDrawn`,
  `castingLeft`, `castingRight`, and `blocking`. Each lasts longer than the
  0.05 s interval, a late joiner sees it, and a lost end cannot leave an actor
  drawing or blocking forever.
- **Ordered actions** for what is short and can repeat faster than the interval:
  swings, bashes, and power attacks. A latest-wins field would lose them.

Hand presentation fields live in `Network::PlayerTransform::handPresentation`
(`Multiplayer::HandPresentation`), not in `AnimationState`, because a change to
`AnimationState` restages locomotion.

## Weapon stance

The source publishes `weaponDrawn`. On the receiver, the actor compares it with
Skyrim's weapon state and, when a transition is needed and none is in progress,
invokes Skyrim's native draw or sheathe. `WantToDraw` and `WantToSheathe` only
mean a request is outstanding, so the request is retried until the actor reaches
`Drawing` or `Sheathing`. The firewall admits the native equip events
([Remote actor](remote-actor.md#animation-firewall)).

- The clip owns the graph until the matching `WeapEquip_Out[Moving]` or
  `Unequip_Out[Moving]` annotation, plus one graph update for its worker-thread
  exit. Readied spell hands draw through the magic equip branch, and
  `Magic_Equip_Out[Moving]` and `Magic_Unequip_Out[Moving]` complete it the same
  way; without them the draw never reached `kDrawn`.
- After the base checkpoint settles, a draw or sheathe is requested and retried
  until it animates. The equip graph picks its standing or moving branch at
  entry from `iSyncIdleLocomotion`, which the receiver holds at the dispatched
  movement ([Locomotion](locomotion.md#standing-re-entry)).
- After an unequip, when graph and actor are both sheathed,
  `IdleForceDefaultState` is forced before locomotion and jump are replayed. It
  is never forced after an equip, which would split the graph's sheathed state
  from the actor's drawn state; `weaponDraw` and `weaponSheathe` only mark the
  attachment and do not close the clip branch. The replay stays pending if the
  transition began idle, so movement that starts after the clip is restored.
- A fallback sends the equip or unequip exit event when a custom graph omits
  the annotation, 2.5 s after the clip starts (a one-handed sword draw takes
  about 1.4 s). A request that flips while the opposite clip still plays
  restarts the wait once the requested clip starts, since timing out mid-sheathe
  resets the graph before its `weaponSheathe` annotation and leaves the weapon
  in hand. Recovery never holds newer checkpoints or translation.
- A draw waits, at most 1 s, until equipment has reconciled the hands, so the
  actor does not draw fists and then pop the weapon in.

## Action stream

Every action carries `actionSequence`, `graphEpoch`, `checkpointSequence`, a
`family`/`action` from a fixed vocabulary, `power` and `direction`, and the
`weaponDrawn` prerequisite. The graph epoch advances whenever the source graph
is rebuilt or loses continuity, so actions from an old graph are never
dispatched into a new one. The checkpoint sequence names a checkpoint published
before the action, which makes the prerequisites reconstructable while
transforms stay latest-wins.

### Capture

`CombatSync::CaptureLocalAttack` watches the player's melee `ATTACK_STATE_ENUM`
every pass, downstream of input mapping:

- An attack starts when the attack window opens, on any change into `kDraw`,
  on entering `kBash` from any state, and on a change into `kSwing` from
  `kNextAttack`, `kFollowThrough`, `kBash` (the bash state accepts
  `attackStart`), or `kHit`.
- The engine writes `kDraw` (or `kBash` for bash data) each time the graph
  accepts an attack event, while annotations only advance a swing:
  `weaponSwing`/`weaponLeftSwing` write `kSwing` from any state, `HitFrame`
  writes `kHit` only from `kSwing`, then `attackWinStart`, `attackWinEnd`, and
  `attackStop` (read from the 1.7.104 executable). A pass can see a chained
  swing only as `kSwing`, so a swing after a hit counts as well: every vanilla
  plain attack clip swings and hits once (`animationdatasinglefile.txt`), and
  attack replacers whose clips have no `attackWinStart` chain from `kHit`.
- Changes into `kSwing` do not count while the attack data is bash data, since
  the dual-wield bash clips (`DW1HM1HMBlockBash`, `DW1HM1HMBlockBashPower`)
  fire `weaponSwing` at their first frame; nor does `kHit` to `kSwing` during a
  power attack, since the vanilla dual-wield special, two-handed three-slash,
  and unarmed forward power attacks swing again after a hit.
- With a bow or crossbow in the right hand only bashes count, since a shot
  passes through `kNextAttack` and `kFollowThrough` too.
- Captured actions keep their order in a bounded queue (32). A captured attack
  flushes equipment first, then the next checkpoint is published and the action
  after it, anchored to that checkpoint and its epoch. A graph epoch change,
  a disconnect, or leaving the world discards captured actions.

The action is chosen from the high process's current `BGSAttackData`, the
race's entry for the graph event that started the attack:

1. `kBash`, or attack data with the bash flag, is `bash`.
2. An event containing `DualWield` is `dual`; one ending in `LeftHand` or
   `H2HLeft` is `left`. `attackPowerStartLeft` is a right-hand power attack
   toward the left, so only these suffixes count.
3. Otherwise the left-attack flag (`BGSAttackData::IsLeftAttack`) selects
   `left`, and anything else, including missing data, is `normal`.

`power` is the attack data's power flag. `attackPowerStartH2HCombo` is `dual`.
The direction comes from the event name: without the `LeftHand` or `RightHand`
suffix, a name containing `Forward` or `Sprint` is `forward`, `Back` is
`backward`, one ending in `Left` or `Right` is that side, and anything else,
including a modded event, is `standing`. `dual` and `bash` are always
`standing`. A bash found from `kBash` is a power bash only if the attack data is
bash data; data left over from a power swing does not count.

### Power attacks on the source

The player never starts a power attack directly. The attack button starts the
plain swing, and every attack edge logged `attackEvent=attackStart` (observed in
game). If the button is still held after `AttackBlockHandler`'s
`initialPowerAttackDelay` (or `subsequentPowerAttackDelay` in a combo), the
engine sends the power event into the playing swing and replaces the attack
data, without a new edge. So while the same attack window stays active, data
that turns into a power attack is captured once more as a power attack, never
again until the next edge and never during a bash. A held attack is therefore a
plain action followed by a power action of the same hand, which keeps plain
attacks as responsive as before.

The log lines `observed local attack state edge` and
`observed local power attack of the playing swing` show the action, power, direction, and event;
the edge line also shows the attack state it left (`previousState`).

### Receiver queue

Actions for a player are accepted only in order, with a matching epoch and a
known checkpoint, into a bounded FIFO (32). The front action waits until:

- its checkpoint has played ([Sync pipeline](sync-pipeline.md#checkpoint-playout))
  and its epoch matches the played checkpoint;
- the graph and the applied locomotion state have settled;
- no jump, landing, draw, equip, recovery, or furniture owns the graph;
- `ActorState` reports the weapon fully drawn;
- the hands have reconciled the latest equipment revision;
- no bow draw or cast plays, and a held or lowering block allows only `bash`;
- the preceding attack has completed or is in its combo window.

A queued action times out after 3 s, a dispatch is retried at most four times,
and a furniture checkpoint discards the queue.

### Catching up

A peer's replaced attack animations (for example player-only replacers) can
play faster than the vanilla NPC clips a remote actor plays, so its swings
arrive before the playing attack reaches its combo window. Chained swings were
measured waiting 0.5–1.2 s with more queued behind them, and behind power
attacks of 1.5–3.3 s a queued attack reached the 3 s timeout and was lost.

- Each queued action counts how long it has been due (`dueElapsed`, from the
  pass its anchor checkpoint played).
- While an attack plays and the front action is due, the actor's weapon speed
  is scaled by `1 + 0.5 × min(due / 0.5 s, 1)`, up to 1.5×, through
  `ActorRegistry::SetAttackSpeedScale`, which the weapon speed channels apply
  ([Remote actor](remote-actor.md#graph-channels)). The vanilla behavior files
  bind the `playbackSpeed` of 110 attack clips, power and NPC variants
  included, to `weaponSpeedMult` and 24 left-hand and dual-wield clips to
  `leftWeaponSpeedMult`, so the swing and its combo window both arrive
  sooner. The scale returns to 1 as soon as nothing waits.
- A plain swing (not power, not a bash) still due after 1 s behind a playing
  attack is dropped, so the actions behind it keep pace; power attacks and
  bashes still only time out.
- Clearing the queue resets the scale; while the actor rides, the rider's scale
  is set back to 1 every pass.
- Logs: `attack catch-up started`, `attack catch-up ended` with the peak speed,
  and `dropped lagging queued attack` with the wait. Late join, graph replacement,
respawn, and hit recovery first restore the current checkpoint; historical
attacks are never replayed.

### Dispatch

The chosen event goes through the firewall bypass with `kAttackingDisabled`
cleared only for that synchronous call. The receiver does not send `moveStop`,
zero the speed, or pause translation, so Skyrim picks its walking or running
attack branch. While an attack owns the graph, jumps and equip changes wait, so
they cannot cut it off. Ownership ends when the actor's melee attack state
returns to `None`, or on the bounded start (1.25 s) and playback (4 s)
fallbacks; grounded locomotion is then reasserted.

| Action | Plain event | Power event by the actor's hands |
| --- | --- | --- |
| `normal` | `attackStart` | right hand holds something: `attackPowerStart{InPlace,Forward,Backward,Left,Right}` by direction; empty: `attackPowerStartForwardH2HRightHand` |
| `left` | `attackStartLeftHand`, or `AttackStartH2HLeft` with an empty left hand | left hand holds something: `attackPowerStart{InPlace,Forward,Back,Left,Right}LeftHand`; empty: `attackPowerStartForwardH2HLeftHand` |
| `dual` | `attackStartDualWield`, falling back to `attackStart` in the same call | either hand holds something: `attackPowerStartDualWield`; both empty: `attackPowerStartH2HCombo` |
| `bash` | `bashStart` | `bashPowerStart` |

- `normal`, `left`, and `dual` chain in the playing attack's combo window. A
  `bash` never chains.
- A power attack of the same hand as the playing network attack powers it up
  (`CanPowerUpAttack`) unless that is a bash or already powered. It does not wait
  for the actor's attack state, which a remote actor reports well into the swing:
  waiting showed about 0.7 s of plain swing before the power attack against
  0.25 s on the source (observed in game). A graph not yet in its attack state
  rejects it and the action is retried. The attack stays owned, and its playback
  timer and attack state observation restart. A rejected power-up has no plain
  fallback, since the plain swing already plays.
- When a plain attack and its power attack are both queued before the plain one
  starts, the plain one is dropped, so the beginning is not shown twice.
- A new power attack rejected outside a combo window is sent again as the plain
  attack of its hand in the same call, including its dual fallback, and logged
  as `presentedAsPlain`. One rejected in a combo window waits for the playing
  attack to finish and then starts.
- A bash also ends on its `bashExit` annotation: a remote actor's attack state
  does not report bashes (observed in game), so every bash otherwise held the
  1.25 s start fallback. Exits seen before the bash was dispatched are ignored.
- Every dispatched power attack is logged with its event, fallbacks, and the
  actor's stamina (`stamina=`).

### Vanilla graph

`1HM_Behavior`, which also hosts the two-handed, bow, and unarmed stances; `IsNPC`
is 1 on a remote actor:

| From | Event | Condition | To |
| --- | --- | --- | --- |
| Ready | `blockStart` | `IsNPC == 1` (another transition covers players) | Block |
| Ready | `attackStartLeftHand` | none | Attack |
| Ready | `AttackStartH2HLeft` | left hand empty, right not a one-handed weapon | Attack |
| Ready | `attackStartDualWield` | both hand types one-handed weapons (1..4) | Attack |
| Ready | `bashStart` | `IsNPC == 1`, or a bow or crossbow in the right hand | Bash |
| Block | `blockStop` | none (two variants on `iBlockState`) | Ready |
| Block | `bashStart`, `bashPowerStart` | `IsNPC == 1` | Bash |
| Bash | `bashExit` (annotation) | `iWantBlock == 1` / `0` | Block / Ready |

`bashStart` plays the whole bash; `BlockBashIntro` and `bashRelease` belong to
the failed-bash branch and are unused. The spell-in-right-hand graph
(`MRh_Shield_OR_Torch_BehaviorGraph`) enters a bash only from its block state,
which is also how the source reached it, and its left weapon graph
(`MRh_1HM_Behavior`) takes `attackStartLeftHand`.

Power attack conditions in the ready state (the sprint variants are not taken):

| Event | Condition |
| --- | --- |
| `attackPowerStart{InPlace,Forward,Backward,Left,Right}` | `iRightHandType > 0` |
| `attackPowerStart{...}LeftHand` | left hand a one-handed weapon (1..4) or a torch (11) |
| `attackPowerStartForwardH2HRightHand` | `iRightHandType == 0`, `bWantCastLeft == 0` |
| `attackPowerStartForwardH2HLeftHand` | `iLeftHandType == 0`, right not a one-handed weapon |
| `attackPowerStartH2HCombo` | both hand types 0 |
| `attackPowerStartDualWield` | both hands weapons other than two-handed, bow, staff, magic, shield, torch |
| `bashPowerStart` | none |

The attack states of `1HM_AttackBehavior` also take their hand's power attacks,
so a power attack can follow a swing in its combo window; power attack states
take no further attack. Each power machine (`AttackPowerFwdBehavior` and the
others) has an NPC and a player variant, and a remote actor plays the NPC one. A
raised block takes only some power attacks, which does not matter, because the
receiver lowers the block before any attack but a bash.

The player race's attack data (`ATKD`/`ATKE`) uses flags `2` (bash), `4`
(power), and `8` (left). Its power events are the table's, plus the sprint
variants `attackPowerStart_Sprint`, `attackPowerStart_2HMSprint`,
`attackPowerStart_2HWSprint`, and `attackPowerStart_SprintLeftHand`.

## Hand presentation

### Capture

Each pass, before equipment capture, `CombatSync::CaptureLocalHandPresentation`
reads the player. Everything is false unless the weapon is drawn:

- `bowDrawn`: a bow in the right hand and `ATTACK_STATE_ENUM` `kBowDraw`,
  `kBowAttached`, or `kBowDrawn`; `kBowReleasing` and later are a release.
- `castingLeft`/`castingRight`: a spell or scroll in that hand and a
  `MagicCaster::state` from the cast request through release (`kUnk01` to
  `kUnk07`); `kNone` is a readied hand that is not casting.
- `blocking`: `Actor::IsBlocking()` without a bow or crossbow in the right hand
  (with a bow the block button zooms, and `BowZoom` sends `blockStart`). `IsBlocking()` is false while a bash plays,
  even from a held block (observed in game), so during `kBash` the player's
  `iWantBlock`, which the vanilla `bashExit` uses to return to the block,
  decides instead, or `ActorState::wantBlocking` without that variable.

Fields are ORed across passes until the next checkpoint samples and clears them,
so a presentation shorter than the interval still reaches peers once. A field
turning on flushes equipment first, so a new spell or bow arrives before its use.

### Receiver stage

`CombatSync::SyncHandPresentation` runs after `SyncCombat` and not while
furniture owns the actor. Each desired state is the checkpoint field while it is
drawn, grounded, and not in furniture. A new block, draw, or cast also needs
settled hands and no melee attack playing, and starts only with a stable graph:
a settled revision, no weapon transition, recovery, attack, locomotion
reassert, or bow shot, a fully drawn `WEAPON_STATE`, and the right object in the
hand. `ApplyBlock` runs before the bow and casts, and a block excludes both.
Starts and rejected events retry at most every 0.1 s. Bow and block events go
through the bypass with `kAttackingDisabled` and `kCastingDisabled` cleared only
for that call. All presentation state is cleared when the graph is invalidated.

While any presentation plays, `AnimationSync` holds the actor's current
`weaponDrawn` and `jumping` while locomotion keeps following the checkpoint, so
a sheathe or jump in the same checkpoint first ends the presentation. A normal
attack cannot start and furniture entry waits.

### Blocking

- **Raise:** `SetBlockRequest(true)` sets `ActorState::wantBlocking` and the
  graph's `iWantBlock`, then `blockStart` is sent. A rejected start clears the
  request.
- **Hold:** the request is set again every pass, so a bash's `bashExit` returns
  to the block.
- **Lower:** the request is cleared and `blockStop` sent. A playing bash rejects
  it and then exits to ready by itself. A block that rejected it is asked again
  every 0.1 s while no attack plays. It is lowered once `IsBlocking` is false, or
  on the accepted event when the graph lacks that variable. After 1.5 s,
  `blockStopInstant` drops it. A request left on the actor after a reset is
  cleared on the next pass.

### Bow

`bowAttackStart` begins the draw, held while the desired state stays on; a held
draw is never dropped without an exit event. When it clears, `attackRelease` is
sent. An early release is rejected until the arrow is drawn and is retried;
after 1.5 s of rejections, `attackStop` then `bowReset` cancel instead. The shot
owns the graph for 0.5 s, and while the attack state still reports a bow attack,
at most 2 s. A remote actor's `ATTACK_STATE_ENUM` does not follow its graph
(observed in game), so nothing depends on it entering the bow states.
Crossbows (`crossbowAttackStart`) are not replicated.

### Spells

A hand holding any spell or scroll holds a stand-in of one placeholder,
Healing Hands (`Skyrim.esm` `0x04D3F2`, a concentration spell aimed at a target
actor), that looks like the source's spell ([Spell look](#spell-look)). A cast
plays Healing Hands' cast from that hand for as long as the source casts,
whatever the source's spell.

- **Cast.** The hand's own `ActorMagicCaster` casts it, as NPC casting does:
  `SetCurrentSpell` if needed, then `RequestCastImpl`. Sending the graph's cast
  events directly failed: the magic graph emits `InterruptCast` for a cast its
  caster did not request and rejects a directly sent
  `M?h_SpellAimedConcentrationStart` (observed in game). `RequestCastImpl` alone
  only sends `BeginCast*` and left the caster in `kNone` (observed in game), so a
  start also moves an idle caster to `kUnk01`, as the engine's request does, and
  sets `bWantCastLeft`/`bWantCastRight`, which the magic graph needs to stay
  casting; the variable is set again every pass while held. The engine then runs
  charge, ready, and cast. `StartCastImpl` performs the hand's release action
  (`ActionLeftRelease`/`ActionRightRelease`), which picks the clip through `IDLE`
  conditions on casting type and delivery; the human aimed branches
  (`RightCastAimed`, `LeftConcentrationAimed`) take delivery `>= 2`, so Healing
  Hands' target-actor delivery (3) plays the aimed concentration clip. The
  caster then waits in state 4, because spell fire is suppressed.
- **Stop.** The variable is cleared and `InterruptCast(false)` performs the
  hand's interrupt action and finishes the cast. `FinishCast` alone returned the
  caster to `kNone` but left the clip looping (observed in game). The stop
  completes once the caster is `kNone` and the hand's `IsCastingLeft` or
  `IsCastingRight` is false, or after 1 s; no new cast starts on that hand
  before. The last hand to stop also waits for the graph's `iState` to leave
  10, the cast branch's tag (spell-ready locomotion reads 9). The graph kept 10
  for about 0.2 s after both variables cleared, and a `moveStart` sent then was
  accepted by the exiting cast lower body, which returned to the standing
  spell-ready state, so the actor slid until its next stop (observed in game).
  A cast also stops when the hand loses its spell.
- While a cast is held or stopping, the registry marks cast ownership, and the
  firewall passes cast events, which the caster sends then and on later updates
  ([Remote actor](remote-actor.md#animation-firewall)). Names are matched
  ignoring case: the left release idle sends `MLH_SpellRelease_event`.
- A dual cast plays the one-handed cast in each hand. Staves stay weapons and
  play no cast.

### Spell look

Goal: a peer recognizes the source's spell in hand by its hand art, hand light,
and hand sounds, while the cast animation and its isolation stay the
placeholder's. Non-goals: the source's casting type, delivery, dual casting,
projectile, beam, hit effects, and release and hit sounds.

The magic caster takes a hand's art (`castingArt`), light (`light`), and sounds
from the costliest effect of the hand's spell (`MagicItem::GetAVEffect`). A
stand-in is a runtime copy of Healing Hands whose effect is a runtime copy of
Healing Hands' effect with that art, light, and the draw/sheathe, charge, ready
loop, and cast loop sounds replaced; the release and hit sounds are left out.
Its casting type, delivery, charge time, and equip slot stay Healing Hands', so
the cast plays and is suppressed exactly as above.

- **Capture.** A spell or scroll hand sends the spell's reference and, as a
  fallback, the costliest effect's casting art and casting light references
  ([Network](network.md#equipment)). An unreferenceable (dynamic) spell is sent
  without a form. If the art or light exists but cannot be referenced, neither
  is sent, since a partial look would read as a spell without that part.
- **Resolution**, in order:
  1. The spell resolves to a `SpellItem` or `ScrollItem`: its art, light, and
     sounds, as the receiver's version, so the receiver's visual mods apply.
  2. At least one of the art and light is sent and each sent one resolves
     (`BGSArtObject`, `TESObjectLIGH`): those, with Healing Hands' sounds; an
     unsent one is none. Many modded spells reuse vanilla art.
  3. Otherwise Healing Hands itself.
- **Stand-ins** are created on the game thread with `DynamicForm::Create` and
  `TESForm::Copy` (`MagicItem::Copy` copies the data and effects, not the equip
  slot; `EffectSetting::Copy` not the sounds), cached by the form IDs of the
  art, light, and four sounds, shared by every hand and peer with that look, and
  never deleted. `Spell stand-in created` logs each one with its forms. The
  stand-in's light is the look's [fake light](appearance-and-equipment.md#fake-lights)
  version.
- A spell change swaps the stand-in through `EquipSpell`, like any hand swap
  ([Appearance and equipment](appearance-and-equipment.md#reconciliation)).

### Moving lower body

The lower bodies of the block (`Blocking_LowerBody_Behavior`), bow draw
(`Bow_Attack_LowerBodyBehavior`), and cast (`DefaultMagicCastingBehavior`) pick
their standing or moving start state from `iSyncIdleLocomotion`, which stayed 0
on a remote actor even while running (observed in game), so a presentation
started while moving entered the standing pose while translation continued.
The receiver holds it at the dispatched movement
([Locomotion](locomotion.md#standing-re-entry)), so the lower body starts in
the right state; later movement reaches it through ordinary locomotion events.

When a presentation ends (the bow after its shot, the block once lowered, the
cast once no hand still casts or stops), the graph returns to the base
locomotion, and a locomotion reassert sends whichever of `moveStart`,
`sneakStart`, and `SprintStart` the graph does not already hold, without
touching the stance. Before the hold the base locomotion re-entered standing,
the actor slid, and the graph then rejected the next `moveStop` until its
retries ran out.

## Isolation

Draw and cast clips natively run the release paths. These hooks keep a
registered actor from releasing anything; other actors pass through. The vtable
hooks install on every runtime. The spell fire entry detour was read from
1.7.104; its SE ID 33629 has the same entry bytes and callees in the 1.5.97
disassembly (2026-10-12, Linux syntax check only, not run on SE).

| Hook | Effect for a remote actor |
| --- | --- |
| `ArrowReleaseHandler::ExecuteHandler` (`arrowRelease`) | Returns handled without launching the arrow or spending ammo |
| Spell fire (ID 34407, entry detour), from `MLh_SpellFire_Event`/`MRh_SpellFire_Event` and a casting caster's update | Returns true without spending cost, applying or launching the spell, or entering casting state 6 |
| `ActorMagicCaster::CastSpellImmediate` (vfunc 1) | Does not cast |
| `ActorMagicCaster::CheckCast` (vfunc 0xA) | Returns true with `kOK`, so magicka or the casting-disabled flag never interrupt |

Hooking the `M?h_SpellFire_Event` handlers did not stop the caster reaching
state 6 (observed in game), so spell fire itself is hooked, and it suppressed
every fire. Melee, bash, and projectile hits are dropped by
[combat isolation](remote-actor.md#combat-isolation).

## Engine assumptions

To verify in game:

1. `high->attackData` holds the starting attack's data when the attack state
   first changes, and turns into the power data within the same window, for
   every hand, dual, unarmed, and sprint attack. A chained held swing is a new
   edge with plain data followed by power data.
2. The attack state enters `kBash` for shield, weapon, torch, bow, and power
   bashes. `iWantBlock` stays 1 during a bash from a held block and 0 otherwise.
3. `IsBlocking()` is true for the whole hold and false during a bow zoom. A
   remote actor accepts `blockStart` for shields, one- and two-handed weapons,
   torches, and fists, its block follows walking and running, and its
   `IsBlocking` variable follows the block state.
4. `bashStart` from a raised block returns to the block with `wantBlocking` and
   `iWantBlock` set; a bow bash plays from ready; the output sink sees `bashExit`
   for every bash (`Observed actor bash exit marker`).
5. `attackStartLeftHand`, `AttackStartH2HLeft`, `attackStartDualWield`, and every
   power event are accepted with matching hands, including a left weapon with a
   spell in the right hand, standing and moving, chain in the combo window, and
   play the NPC variant (`poweredUpPlayingAttack=true` for power-ups).
6. A power attack's attack state returns to `kNone` before the 4 s fallback, an
   actor without stamina still plays it, a power bash returns to the block, the
   forward power attack stands in acceptably for a sprinting one, and the clip's
   own motion does not fight the network translation during a lunge.
7. `MagicCaster::state` is `kNone` while a spell hand is readied and stays in
   `kUnk01..kUnk07` for the whole hold of concentration and fire-and-forget
   spells (`observed local hand presentation`).
8. `UnequipSpell` empties only the given hand and lowers it through
   `ActionForceEquip` (`WeapOutRightReplaceForceEquip`, `WeapSoloEquip`, or
   `Magic_Solo_Equip`). `DrawWeaponMagicHands(true)` readies spell hands without
   other blocked `Magic*` or `M?h_*` events.
9. The cast works from the left hand, dual casting, and while walking or
   running. If a beam or heal still appears, extend suppression to
   `MagicCaster::FindTargets`.
10. The graph takes `attackRelease` and locomotion events during a draw, the
    release clip ends within 0.5 s, `ArrowReleaseHandler` is the only launch path,
    and the release clip's own annotations still remove the nocked arrow.
11. The hand art (ID 38784 via `GetAVEffect`), light
    (`ActorMagicCaster::CheckAttachCastingArt`), and sounds
    (`ActorMagicCaster::PrepareSound`, ID 34438) come only from the selected
    spell's `GetAVEffect` (read from 1.7.104), so a stand-in shows the source's
    look with Healing Hands' clips, and swapping stand-ins on a drawn hand
    replaces the art and light.
12. The copied stand-in plays the same cast clips as Healing Hands, and the
    runtime forms stay valid across save loads, as runtime actor bases do
    ([Appearance and equipment](appearance-and-equipment.md#actor-base)).

## Acceptance criteria

1. A remote actor converges to the source's drawn or sheathed stance without
   local combat AI or local attack events, also when stance packets are dropped.
   A graph rebuild restores the stance before any queued action is dispatchable.
2. Every locally accepted swing, bash, and power attack plays once on peers in
   the source's order, with the same hand, power, and direction, standing or
   moving; one the peer's graph cannot play shows the plain attack of that hand.
3. An attack waits for jump, landing, and draw completion and cannot interrupt
   them; a later jump, equip, or attack does not interrupt an accepted attack.
   Moving actors keep locomotion and translation while attacking.
4. Blocking with a shield, a weapon, a torch, or fists raises the block for as
   long as the source holds it; a bash from a held block returns to it; a swing
   right after lowering plays after the block is lowered. A bow zoom is not a
   block.
5. Drawing, holding, and releasing a bow plays the same on peers with the
   source's arrows in the quiver; nothing is fired or spent.
6. A spell or scroll hand shows the source spell's hand art and light while
   drawn and plays its draw, charge, and loop sounds; a peer without the spell
   shows its art and light if it has those, and Healing Hands' look otherwise. A
   cast plays Healing Hands' cast in that hand for as long as the source casts;
   no projectile, beam, effect, or sound of a released spell appears.
7. Sheathing, jumping, or sitting ends a draw, cast, or block first. A late
   joiner sees an ongoing draw, cast, or block.
8. Nothing is hit, staggered, or damaged.

## Known limitations

- The source's casting type, delivery, and dual casting are not shown: every
  cast plays the aimed concentration clip. A fire-and-forget spell has no cast
  loop sound, so a held cast may be quieter than the source's held charge.
- A peer without the spell plays Healing Hands' sounds with the fallback art.
- Crossbow attacks and bashes, shield charge (`BlockBashSprint`), block hit
  reactions, bash failures, sprint power attacks as their own animation (they
  start from idle records with perk conditions,
  `AttackRightForwardSprinting_2hand` and its children, which the action stream
  does not replicate), kill moves, and creature or werewolf attacks are not
  replicated.
- A cancelled bow draw shows as a release.
- A replacer clip that swings again after a hit in a plain attack is captured
  as a chained attack, and a chained replacer swing that writes `kSwing`
  without a hit in between, with no pass seeing `kDraw`, is not captured.
- Stamina, perks, power attack slow motion, damage, and targeting are out of
  scope.
