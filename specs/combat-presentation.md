# Combat Presentation

## Status

Dual casts ([Spells](#spells)) have not been confirmed in game. The isolation
of a native hand's spell from the local world ([Spell world
isolation](#spell-world-isolation)) has not run in game.

## Scope

A remote actor shows its source's weapon stance, melee and bow attacks,
bashes, power attacks, blocking, bow draws, and spell casts, standing or
moving. Only animations and hand effects play, except a spell the receiver
also has: it casts natively, so its projectiles and beams show in flight in
the receiver's world, and its self effects apply to the remote actor
([Spells](#spells)). Nothing is hit, staggered, damaged, pushed,
spent, summoned, or targeted, and no arrow is launched
([Spell world isolation](#spell-world-isolation)).

The wire fields are in [Network](network.md#checkpoint-and-context) and
[Network](network.md#actions); the synchronized weapons, spell hands, and
ammo are in [Appearance and equipment](appearance-and-equipment.md#equipment);
isolation of hits is in [Remote actor](remote-actor.md#combat-isolation).

Graph event names, transitions, and conditions come from the vanilla
`Skyrim - Animations.bsa` behavior files (`0_master.hkx`, `1hm_behavior.hkx`,
`blockbehavior.hkx`, `bashbehavior.hkx`, `WerewolfBehavior.hkx`,
`VampireLord.hkx`) and the `Skyrim.esm` and `Dawnguard.esm` idle, race, and
attack data records.

## Components

| Module | Responsibility |
| --- | --- |
| `sync/combat/CombatSync` | Source attack and hand-presentation capture; receiver action queue, dispatch gating, and hand presentation stage |
| `sync/animation/AnimationSync` | Weapon stance as part of the presentable state; graph epoch |
| `actor/AttackDispatch` | Attack gating (`CanStart`, `CanChain`, `CanPowerUp`), `Dispatch`, and attack ownership (`Own`, `Advance`) |
| `actor/HandPresentation` | `ApplyBlock`, `ApplyBowDraw`, `ApplySpellCast`, and their ownership of the graph |
| `actor/WeaponTransition` | The native draw and sheathe request and its completion ([Locomotion](locomotion.md#components)) |
| `actor/ActorAnimation` | `GetCombatReadyState`, the graph and stance test every attack and hand presentation start shares |
| `sync/equipment/SpellStandIn` | Spell hand look capture, resolution, and the stand-in spells |
| `actor/SpellIsolation` | Spell fire and cast hooks, and the hooks that keep a remote actor's magic, projectiles, explosions, and hazards from acting on the local world |
| `actor/AnimationEvents.h` | Event names and `GetPowerAttackEventDirection` |
| `actor/BeastActions` | The beast action vocabulary and its graph events |
| `actor/ActorShapeshift` | Beast action dispatch, vampire lord levitation and cast requests |
| `runtime/PlayerGraphEventHook` | The local player's graph input events |
| `sync/shapeshift/ShapeshiftSync` | Beast action capture and publishing; receiver queue and dispatch gating |

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

The source publishes `weaponDrawn` as the direction of its weapon clip: true
from `Drawing` through `WantToSheathe`, false from `Sheathing` through
`WantToDraw`. `ActorState::IsWeaponDrawn` changes only when a clip ends, which
would start every peer's draw or sheathe a whole clip late, so a jump right
after a sheathe would cut into the peer's sheathe. On the receiver, the actor compares it with
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
  in hand. For the same reason the wait restarts while a jump or its landing
  owns the graph: a takeoff interrupts the clip while `WEAPON_STATE` still
  reports it, and the landing exit plays the clip again from its start.
  Recovery never holds newer checkpoints or translation.
- A draw waits, at most 1 s, until equipment has reconciled the hands, so the
  actor does not draw fists and then pop the weapon in.

## Action stream

Every action carries `actionSequence`, `graphEpoch`, `checkpointSequence`, a
`family`/`action` from a fixed vocabulary, `power` and `direction` (one
`Attack.direction` on the wire, `NONE` unless a power attack,
[Network](network.md#actions)), and the `weaponDrawn` prerequisite. The graph epoch advances whenever the source graph
is rebuilt or loses continuity, so actions from an old graph are never
dispatched into a new one. The checkpoint sequence names a checkpoint published
before the action, which makes the prerequisites reconstructable while
transforms stay latest-wins. Beast actions and
[idle events](idle-presentation.md#state-and-actions) share the stream and its
sequence.

### Capture

`CombatSync::CaptureLocalAttack` watches the player's melee `ATTACK_STATE_ENUM`
every pass, downstream of input mapping:

- An attack starts when the attack window opens, on any change into `kDraw`,
  on entering `kBash` from any state, and on a change into `kSwing` from
  `kNextAttack`, `kFollowThrough`, `kBash` (the bash state accepts
  `attackStart`), or `kHit`.
- An accepted attack event and the swing it starts are one attack. After an
  attack starts at `kDraw`, neither `kDraw` nor `kSwing` starts another until
  the attack state has been `kSwing`: a vanilla graph accepts the next attack
  right after the hit and swings only after `attackWinStart` (observed in game
  with replaced clips), so that swing belongs to the attack already captured.
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
attacks as responsive as a click.

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
- the preceding attack has completed or its swing has hit.

A queued action times out after 3 s, a dispatch is retried at most four times,
and a furniture checkpoint discards the queue. Late join, graph replacement,
respawn, and hit recovery first restore the current checkpoint; historical
attacks are never replayed.

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
  and `dropped lagging queued attack` with the wait.

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

- `normal`, `left`, and `dual` chain into a playing attack whose swing has hit
  (`kHit`, `kNextAttack`, or `kFollowThrough`); the graph decides when it takes
  the event, and a rejected chain is sent again on the next pass. A vanilla
  graph accepts it after the hit and swings after `attackWinStart`, while
  MCO-style graphs, whose clips have no `attackWinStart`, never report
  `kNextAttack` or `kFollowThrough`. Only one chain waits at a time: the next
  may follow once the attack state reaches `kSwing` (or `kDraw`) again. A
  `bash` never chains.
- The actor's attack state changes while an attack is owned are logged
  (`remote attack state changed`, with the chain rejections so far), and the
  completion line counts the chains, rejections, and a chain still waiting
  for its swing.
- A power attack of the same hand as the playing network attack powers it up
  (`AttackDispatch::CanPowerUp`) unless that is a bash or already powered. It does not wait
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
  does not report bashes (observed in game), so every bash would otherwise hold the
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
  `kUnk07`); `kNone` is a readied hand that is not casting. The release and
  dual cast fields are in [Spells](#spells).
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

Goal: a peer sees the native cast of the source's spell: its charge, held
ready, release, or concentration loop for its casting type and delivery, a
ward, telekinesis, or ritual clip, and a dual cast. When the peer has the
spell, it also sees what the spell does when it fires, as a native NPC cast:
its self effects on the caster, and its projectiles, beams, and sprays in
flight. None of it acts on or marks the peer's world
([Spell world isolation](#spell-world-isolation)).

A hand holding a fire-and-forget or concentration `SpellItem` that the
receiver has holds the receiver's own version of that spell (a native hand),
whose own data picks the clips and whose fire runs natively. Any other spell
or scroll hand, and every hand of a vampire lord, holds a stand-in, a runtime
copy of Healing Hands (`Skyrim.esm` `0x04D3F2`) carrying the source spell's
look ([Spell look](#spell-look)) and cast shape, whose fire stays suppressed
([Isolation](#isolation)). The cast clips come from the
vanilla idle records: `StartChargeImpl` performs the hand's attack action
(`ActionLeftAttack`/`ActionRightAttack`), whose children under
`AttackMagicLeftRoot`/`AttackMagicRightRoot` test `GetCurrentCastingType`,
`GetCurrentDeliveryType`, `SpellHasKeyword` (`MagicWard`, `MagicTelekinesis`,
`RitualSpellEffect`, `RitualSpellIllusion`), and `IsDualCasting`, and send the
charge event (for example `MRh_SpellAimedStart`, `MLh_WardStart`,
`DualMagic_SpellSelfStart`, `RitualSpellStart`). `StartReadyImpl` performs the
ready action and `StartCastImpl` the release action
(`MLh_SpellRelease_Event`/`MRh_SpellRelease_Event`). So the held spell's
casting type, delivery, charge time, and effect keywords pick the clips
natively. `SpellHasKeyword` reads the keywords of the spell's effects, since
vanilla spell records carry none of their own. A two-handed (ritual) spell
casts on the left caster alone, and the left root's ritual branch tests the
right hand's keyword (`SpellHasKeyword` with hand 1), which the right hand's
spell satisfies, so the left cast plays `RitualSpellStart` or
`RitualSpellAimConcentrationStart`.

- **Capture.** Besides `castingLeft`/`castingRight`
  ([Capture](#capture)), `castReleasedLeft`/`castReleasedRight` are set while
  that caster is past `StartCast` (`kUnk04` through `kUnk07`): a
  fire-and-forget hold was released, or a concentration cast started its loop.
  `dualCasting` is the player's `Actor::IsDualCasting` while the left hand
  casts: `Actor::TryDualCast` (ID 38762) runs a dual cast, and a two-handed
  spell's cast, on the left caster. All three are ORed like the other fields.
- **Cast.** The hand's own `ActorMagicCaster` casts it, as NPC casting does:
  `SetCurrentSpell` if needed, then `RequestCastImpl`. Sending the graph's cast
  events directly failed: the magic graph emits `InterruptCast` for a cast its
  caster did not request and rejects a directly sent
  `M?h_SpellAimedConcentrationStart` (observed in game). `RequestCastImpl` alone
  only sends `BeginCast*` and left the caster in `kNone` (observed in game), so a
  start also moves an idle caster to `kUnk01`, as the engine's request does, and
  sets `bWantCastLeft`/`bWantCastRight`, which the magic graph needs to stay
  casting; the variable is set again every pass while held. A start also sets
  the caster's `kSkipCheckCast` (`SetSkipCheckCast`), so the receiver's copy of
  the source's magicka never ends a cast the source holds: the caster update
  (ID 34143) otherwise drains magicka over a fire-and-forget charge and a
  concentration cast, and interrupts a charge whose remaining cost exceeds the
  actor's magicka. A runtime actor base has only its race's magicka, which a
  held ward drains: without the flag, a fire-and-forget charge beside a ward
  was back in `kNone` before the source's release (observed in a replay). The
  engine then
  charges for the spell's charge time and readies. A concentration caster
  releases itself once ready. A native one then casts, firing from the caster
  update; a stand-in waits in `kUnk04`, because its fire is suppressed.
- **Dual cast.** A left cast with `dualCasting`, both hands holding the same
  spell, sets the high process `isDualCasting` and the caster's dual flag
  first, as `Actor::TryDualCast` and `MagicCaster::RequestCast` (ID 34401) do,
  so the attack action takes the `CastDualMagic` branch. Any other left cast
  clears both, since a graph reset can drop a cast without the
  `FinishCastImpl` that clears them.
- **Release.** A fire-and-forget caster waits in `kReady` after its ready
  action. Once the checkpoint reports the hand released, the receiver does
  what `MagicCaster::StartCast` (ID 34445) does for the player's release and
  NPC combat: enters `kUnk04` and calls `StartCastImpl`. It stops setting the
  want-cast variable then. A release that arrives while the receiver still
  charges waits for its ready state, at most `kReleaseChargeWaitSeconds` (1 s),
  even after the source's cast ended, since the receiver's charge started later
  by its start's delay. The release clip's fire event reaches spell fire, which
  for a native hand fires and finishes the cast, and for a stand-in is
  suppressed by a hook that finishes the cast as native spell fire does; the
  caster returning to `kNone` ends the hand's cast without an interrupt. So
  does a fire-and-forget caster that returns to `kNone` by itself, as when the
  engine releases at once because the ready action was rejected. Whether the
  cast is fire-and-forget is read from the spell when the cast starts: a
  native cast's finish clears the caster's `currentSpell`, so reading it then
  held every native cast for the fallback (observed in a replay). After
  `kReleaseFinishFallbackSeconds` (3 s) without that, the cast is interrupted.
- **Stop.** A cast the source ended without a release, a concentration cast, or
  a hand that lost its spell is stopped: the variable is cleared and
  `InterruptCast(false)` performs the hand's interrupt action and finishes the
  cast. `FinishCast` alone returned the caster to `kNone` but left the clip
  looping (observed in game). So a charge the source cancels is cancelled.
- **Ending.** The stop or released cast completes once the caster is `kNone`
  and the hand's `IsCastingLeft` or `IsCastingRight` is false, or after 1 s; no
  new cast starts on that hand before. The last hand to stop also waits for the
  graph's `iState` to leave 10, the cast branch's tag (spell-ready locomotion
  reads 9). The graph kept 10 for about 0.2 s after both variables cleared, and
  a `moveStart` sent then was accepted by the exiting cast lower body, which
  returned to the standing spell-ready state, so the actor slid until its next
  stop (observed in game).
- **No repeat.** A fire-and-forget cast that ended, released or interrupted,
  while the checkpoint still reports the hand casting and released, is not
  started again; the next cast starts after a checkpoint reports the hand idle
  or casting without a release.
- While a cast is held or stopping, the registry marks cast ownership, and the
  firewall passes cast events, which the caster sends then and on later updates
  ([Remote actor](remote-actor.md#animation-firewall)). Names are matched
  ignoring case: the left release idle sends `MLH_SpellRelease_event`.
- Staves stay weapons and play no cast.

### Spell look

Goal: a peer recognizes the source's spell in hand by its hand art, hand light,
and hand sounds. A native hand has them as its own; this section covers the
stand-in, which also leaves out the release and hit sounds, the projectile,
the beam, and the hit effects.

The magic caster takes a hand's art (`castingArt`), light (`light`), and sounds
from the costliest effect of the hand's spell (`MagicItem::GetAVEffect`). A
stand-in is a runtime copy of Healing Hands whose effect is a runtime copy of
Healing Hands' effect with that art, light, and the draw/sheathe, charge, ready
loop, and cast loop sounds replaced; the release and hit sounds are left out.
It also carries a cast shape ([Spells](#spells)): the casting type
(concentration, or fire-and-forget for anything else, including a scroll's),
delivery, and charge time on the spell and its effect, and the keywords of all
the spell's effects on its effect. The charge time goes into the effect's
`spellmakingChargeTime` too, because `SpellItem::GetChargeTime` (ID 11496)
returns the largest of those for an auto-calculated spell such as Healing
Hands, whose effects have none, and the spell's own charge time only otherwise. Its equip slot and effect archetype stay
Healing Hands', so it equips in either hand and nothing an archetype does on
fire applies.

- **Capture.** A spell or scroll hand sends the spell's reference and, as a
  fallback, the costliest effect's casting art and casting light references
  ([Network](network.md#equipment)). An unreferenceable (dynamic) spell is sent
  without a form. If the art or light exists but cannot be referenced, neither
  is sent, since a partial look would read as a spell without that part.
- **Resolution** (`SpellStandIn::Resolve`), in order:
  1. Outside a vampire lord's hands, the spell resolves to a `SpellItem` of
     type spell, fire-and-forget or concentration: the hand holds it, the
     receiver's version (`native`).
  2. It resolves to another `SpellItem` or a `ScrollItem`: a stand-in with its
     art, light, sounds, and cast shape, as the receiver's version, so the
     receiver's visual mods apply. A scroll keeps a stand-in because
     `EquipSpell` takes only a `SpellItem` and a scroll's cast consumes it.
  3. At least one of the art and light is sent and each sent one resolves
     (`BGSArtObject`, `TESObjectLIGH`): a stand-in with those, Healing Hands'
     sounds, and its cast shape; an unsent one is none. Many modded spells
     reuse vanilla art.
  4. Otherwise a stand-in with Healing Hands' look and shape, never Healing
     Hands itself, which would cast natively. If no stand-in can be created,
     the hand is empty.
- **Stand-ins** are created on the game thread with `DynamicForm::Create` and
  `TESForm::Copy` (`MagicItem::Copy` copies the data and effects, not the equip
  slot; `EffectSetting::Copy` neither the sounds nor the keywords), cached by
  the form IDs of the art, light, four sounds, and keywords, the casting type,
  delivery, and charge time, shared by every hand and peer with that look and
  shape, and never deleted. `Spell stand-in created` logs each one with its
  forms and shape. The stand-in's light is the look's
  [fake light](appearance-and-equipment.md#fake-lights) version.
- A spell change swaps the held spell through `EquipSpell`, like any hand swap
  ([Appearance and equipment](appearance-and-equipment.md#reconciliation)).

### Moving lower body

The lower bodies of the block (`Blocking_LowerBody_Behavior`), bow draw
(`Bow_Attack_LowerBodyBehavior`), and cast (`DefaultMagicCastingBehavior`) pick
their standing or moving start state from `iSyncIdleLocomotion`, which no
vanilla node writes, so on a remote actor a presentation started while moving
would enter the standing pose while translation continued (observed in game).
The receiver holds it at the dispatched movement
([Locomotion](locomotion.md#standing-re-entry)), so the lower body starts in
the right state; later movement reaches it through ordinary locomotion events.

When a presentation ends (the bow after its shot, the block once lowered, the
cast once no hand still casts or stops), the graph returns to the base
locomotion, and a locomotion reassert sends whichever of `moveStart`,
`sneakStart`, and `SprintStart` the graph does not already hold, without
touching the stance. Without the hold the base locomotion re-enters standing,
the actor slides, and the graph then rejects the next `moveStop` until its
retries run out.

## Beast actions

A player in a [beast form](appearance-and-equipment.md#shapeshifts) attacks,
howls, feeds, and casts through the beast's own vanilla graph, whose events
differ from the human ones. Peers replay the graph input events the source's
graph accepted, from a fixed vanilla vocabulary per graph, as the `beast`
value of an ordered `Action` ([Network](network.md#actions)). Only animations
and the sounds their clips annotate play: nothing is hit, summoned, fed on,
or drained.

### Vocabulary

`BeastActions` maps each value to one event, sent by the beasts' attack data
(race `ATKE`), IDLE records, and casts:

| Graph | Actions |
| --- | --- |
| werewolf (also the werebear) | the race's 14 attack events (`AttackStartRight`, `AttackStartLeft`, their power, side, running power, and sprinting variants, `AttackStartDual`, `AttackStartDualRunning`, `AttackStartDualSprinting`, `AttackStartBackHand`), `HowlStart`, `HowlRelease`, `HowlEnd`, `SpecialFeeding`, `pa_KillWerewolfPairedFeedingWithHuman` |
| vampire lord | `AttackStartRight`, `AttackStartLeft`, `AttackPowerStart_Right`, `AttackPowerStart_Left`, `BatSprintStart`, `batSprintStop`, `pa_KillMoveVampireLordFeedFront`, `pa_KillMoveVampireLordFeedBack`, and per hand `BeginCast*`, `M?h_SpellAimedStart`, `M?h_SpellAimedConcentrationStart`, `M?h_SpellReady_Event`, `M?h_SpellRelease_Event`, `M?h_Equipped_Event` (the interrupt idle) |

Locomotion, weapon stance, and levitation are state, not actions: the
checkpoint carries them. The werewolf's `AttackStartHowlExplode` attack data
has no transition in its graph and is left out.

### Capture

- `PlayerGraphEventHook` wraps `PlayerCharacter`'s
  `IAnimationGraphManagerHolder::NotifyAnimationGraph` (vtable 3, vfunc 1) and
  passes every accepted event to `ShapeshiftSync::ObserveLocalGraphEvent`, on
  whichever thread raised it. That returns at once unless the local form is a
  beast (an atomic the main loop sets), matches the event against the form's
  vocabulary by pooled `BSFixedString`, and queues it, at most 32.
- Each pass `QueueLocalActions` moves them to the publishing queue, cleared
  when the graph epoch changes; after the next checkpoint is published,
  `PublishLocalActions` publishes the ones of the checkpoint's form anchored to
  it, so an action never precedes its checkpoint. Leaving the world or the
  connection discards them.
- In a beast form, the human [attack capture](#capture) result is dropped.

### Receiver

`ShapeshiftSync::AgeRemoteActions` ages the queue every pass, also while the
actor respawns in the new form, and drops actions older than 3 s.
`ShapeshiftSync::SyncBeast` runs each pass for an on-foot beast actor outside
furniture:

- The front action waits for its epoch and anchor checkpoint to play. Then
  one for another graph than the played checkpoint's form is dropped; a graph
  epoch boundary drops the old epoch's.
- It waits for the actor to be ready in a form that plays its graph, then
  `ActorShapeshift::DispatchAction` sends its event past the firewall, with
  the replica policy's attacking and casting flags open for that call only, as
  for a human attack. One
  action per pass, so a burst never stacks edges in one graph update. A
  rejected event is retried at most 4 times, 0.25 s apart.
- An accepted attack (`Multiplayer::IsBeastAttack`: the werewolf attacks and
  the vampire lord's melee) owns the graph as a human attack's
  [dispatch](#dispatch) does (`AttackDispatch::Own`), so
  locomotion waits until its attack state ends and is then reasserted. Without
  it, the werewolf graph's `CombatAttacks` state rejects `moveStart` and
  `moveStop` over 100 times per attack and holds translation (seen in game).
- A beast actor runs none of the human attack queue or hand presentation.

### Levitation

The vampire lord hovers or stands, which `H2HBehavior` selects
(`Ground_State_Cam` or `LevitationBase_State`, toggled by `LevitationToggle`
through `Levitate_Start` and `Levitate_End`). The source publishes
`levitating` from its own graph's [levitation
selector](locomotion.md#beast-graphs). `ActorShapeshift::ReconcileLevitation`
reads the actor's selector every 0.1 s and, after 0.3 s of disagreement,
sends `LevitationToggle`, or `LevitationToggleMoving` while moving, as the
vampire lord's toggle idles do: at most 3 toggles per published change,
1.5 s apart. A late joiner thus starts in the source's stance.

### Vampire lord casts

The vampire lord's cast branch, like the human magic graph's, stays active
only while `bWantCastLeft` or `bWantCastRight` is set.
`ActorShapeshift::ApplyCastRequests` writes both from the checkpoint's
`castingLeft` and `castingRight` on every pass of a cast and once after it.
The relayed cast events start and stage the cast.

The actor holds stand-ins of the source's vampire lord spells, sent as
`beast_right_hand` and `beast_left_hand`
([Appearance and equipment](appearance-and-equipment.md#shapeshifts)), so
its hands show their art and light and play their hand sounds as a human
[spell look](#spell-look) does (the stand-ins' cast shape is not used: the
relayed events, not the caster, drive these casts); the magic caster attaches the art once the
hand has a selected spell, a casting art, and a magic node
(`ActorMagicCaster::CheckAttachCastingArt`, ID 34185, from its `Update`,
read in the 1.7.104 disassembly). The beam, projectile, and effects stay
behind the [isolation](#isolation) hooks.

### Mist Form and bats

Both are vanilla magic effects on the vampire lord, not graph events
(`Dawnguard.esm`): `DLC1MistformEffect` shows the hit shader
`DLC1VampireMistform01FXS` and the hit art `DLC1VampMistFormFX`, and its
scripts add `DLC1VampireMistform02FXS` and set the actor's alpha to 0.25;
`DLC1BatsEffect` shows `DLC1VampireBatsFXS` and `DLC1VampLordBatsFX`, and
the relayed `BatSprintStart` plays the dash. `Forms::ResolveBeastForms`
resolves these at `kDataLoaded`.

- The source sets `mistForm` and `bats` while an active effect of the
  player, neither inactive nor dispelled, has that base effect
  (`ShapeshiftSync::SampleLocalVampireLord`, once per checkpoint, only as a
  vampire lord).
- `ActorShapeshift::ApplyVampireLordPowers` applies the shaders
  (`ApplyEffectShader`) and art (`ApplyArtObject`) without a duration, and the
  Mist Form alpha (`Actor::SetAlpha`), when a flag rises on a ready, 3D-loaded
  vampire lord actor, and ends them when it falls. The engine owns the
  reference effects, so the end finds them again through `ProcessLists` by
  target handle and form and marks them finished. A change the actor cannot
  show yet is tried again on the next pass; a respawn starts without them.

## Isolation

Draw and cast clips natively run the release paths. These hooks keep a
registered actor from releasing anything but a native hand's spell; other
actors pass through. The arrow hook is in `CombatIsolation`, the others in
`SpellIsolation`. The vtable hooks install on every runtime; the spell fire
entry detour is ID 34407 (SE 33629).

| Hook | Effect for a remote actor |
| --- | --- |
| `ArrowReleaseHandler::ExecuteHandler` (`arrowRelease`) | Returns handled without launching the arrow or spending ammo |
| Spell fire (ID 34407, entry detour), from `MLh_SpellFire_Event`/`MRh_SpellFire_Event` and a casting caster's update | A native hand's spell runs the original: cost, application or launch, casting state 6 (`Remote actor native spell fire`, throttled). A stand-in returns true without spending cost, applying or launching the spell, or entering casting state 6; a cast that is not concentration, running on a caster under cast ownership ([Spells](#spells)), is finished with `FinishCast`, as the skipped `FindTargets` (ID 34410) ends it |
| `ActorMagicCaster::CastSpellImmediate` (vfunc 1) | Does not cast, except on the caster whose native spell fire is running on that thread (a `thread_local`), in case the fire applies a self spell through it |
| `ActorMagicCaster::CheckCast` (vfunc 0xA) | Returns true with `kOK`, so magicka or the casting-disabled flag never interrupt |

The hooks tell the two apart by the caster's `currentSpell`: stand-ins are
dynamic forms, and nothing else gives a remote actor a dynamic spell, since a
dynamic spell is sent without a form and resolves to a stand-in. So the check
is `IsDynamicForm`, which needs no lock on the threads that run spell fire.

Hooking the `M?h_SpellFire_Event` handlers did not stop the caster reaching
state 6 (observed in game), so spell fire itself is hooked. Melee, bash, and
arrow hits are dropped by [combat isolation](remote-actor.md#combat-isolation).
The magic firewall keeps hostile and detrimental effects from local sources
off remote actors ([Remote actor](remote-actor.md#survival-and-ai-policy)).

### Spell world isolation

Goal: a native hand's spell shows its cast, its flight, and its effects on
the caster, and leaves no trace in the receiver's world. Projectiles, beams,
and sprays fly and vanish where they strike, without an explosion, impact
art, particles, decal, sound, or light; a rune is never launched, and a wall
spell places no flames. Self effects that stay on the caster (a flesh spell's
shader, Candlelight, invisibility, a cloak's aura, a ward, and a hazard cast
around the caster, such as Blizzard or Circle of Protection) apply to the
remote actor. No local actor, the local player, or any object takes damage,
a hit, a stagger, an effect, a push, or destruction; no creature is summoned
or reanimated, and no weapon is bound.

`SpellIsolation` drops each part where the engine applies it, so the fire,
launch, and flight stay native. A source is remote when it is a registered
actor, or a projectile, explosion, or hazard acting for one: a projectile's
`shooter` or `actorCause`, an explosion's `actorOwner` or `actorCause`, a
hazard's `ownerActor`, or any reference's `GetActorCause`.
`Hazard::SetActorCause` (ID 43966) stores its cause's actor as `ownerActor`,
so a hazard cast around the caster acts for it.

| Hook (1.7.104, SE) | Drops for a remote source |
| --- | --- |
| `MagicTarget::AddTarget` (vfunc 1) of `Character` and `PlayerCharacter` (both ID 38786, SE 37832, chained after the [magic firewall](remote-actor.md#survival-and-ai-policy)) and `NonActorMagicTarget` | Any effect on another actor or an object, and on the caster itself any archetype but value modifiers (`kValueModifier`, `kDualValueModifier`, `kPeakValueModifier`, `kValueAndParts`, `kAccumulateMagnitude`), `kInvisibility`, `kLight`, `kNightEye`, `kEtherealize`, `kDisguise`, `kCloak`, and `kSpawnHazard`. Returns false, as the magic firewall does |
| Projectile spell impact (ID 34412 + 9, SE 33634 + 9) `(caster, location, projectile, target, …)` | The spell where a projectile struck. Every impact casts through it with the shooter's own caster: `Projectile::ProcessImpacts` (ID 44123), `HandleHits` (ID 44204, beams and flames) and the cone and arrow impacts through ID 44206, and a lobber's detonation through ID 34411. This includes target-location summons |
| HitData processing (ID 38586, SE 37633) `(target, hitData)` | A hit whose `aggressor` is remote: damage, stagger, hit impulse, hit effects, `TESHitEvent`. Spell projectiles (ID 44123 + 0x269), beams (ID 44204 + 0x3AA), and explosions (ID 43849 + 0x412) reach it without the actor hit hook |
| `TaskQueueInterface::QueueUpdateDestructibleObject` (ID 36909, SE 35934 + 9) | Destruction damage whose cause is a remote source |
| Projectile impact impulse (ID 44215 + 9, SE 43024 + 9) | The push on the rigid body a projectile struck (`ScaleGameplayImpulseForce`, ID 26004, then the body's point impulse) |
| Explosion targets (ID 43849, SE 42677) | Every reference the explosion found: actor cause, HitData, the enchantment, and the impulse (ID 39895). Sets `kTargetsProcessed`, as the original does first, so the update does not call it again |
| Explosion forces (ID 43850, SE 42678) | The impulses on bodies in the radius (`SetLinearImpulse`/`SetAngularImpulse`, IDs 78091/78092). Sets `kForcesApplied`, as the original does |
| Spell projectile launch (ID 34452, SE 33672), from `FindTargets` and the area and target-location casts | Returns true without launching when the current spell's projectile (`GetAVEffect`) has `kExplosionAltTrigger` and an `explosionProximity`: a rune. The callers only test the result, which the original always sets; `Projectile::Launch` (ID 44108) is not failed instead, because this caller uses the new projectile without a null check |
| Impact effects (ID 44213, SE 43022) `(projectile, target, impact, …)` | The impact data set's art, particles, decal, sound, and light, the hit reference's actor cause, and the hazard an impact places (a wall's flames). Its callers are `ProcessImpacts` (+0x407), the beam and flame hits (ID 44205), and the cone impact (ID 43806) |
| Explosion factory call in the projectile update (ID 44216 + 0x3C8, SE 43025 + 0x39F, call detour) | Returns no explosion for a remote projectile, read from the creation record at +0x20, where the update stores it. A null explosion is an ordinary result there, and the projectile is killed as after one |

The explosion target and force hooks cover explosions a remote source creates
outside the projectile update, such as those of a hazard cast around the
caster. A rune's proximity detonation (ID 44217, and
`GrenadeProjectile::OnTriggerEnter`, ID 43933) is not hooked: a remote rune is
never launched, and any other remote projectile that detonates vanishes
without an explosion or a spell.

The ID 34412 and 44215 detours sit past an entry guard (`test r8/r9, r9; je`
to `ret`), as does SE's 35934 (`test rdx, rdx; je`), which 1.7.104 tests after
its `sub rsp`. Each logs `Suppressed remote spell world effect` with the path
and both form IDs, and the magic target hooks `Suppressed remote actor magic
effect` with the archetype, throttled.

The self archetypes are an allowlist rather than a list of world-changing
ones, so a modded archetype use defaults to dropped: a script effect
(`kScript`, such as Transmute), a summon, a bound weapon (equipment sync owns
the hands), telekinesis, or a transformation does nothing. A cloak's and a
spawned hazard's own spells reach targets through new `AddTarget` calls with
the remote actor or the hazard as their source, so the aura and the hazard
around the caster show while their effects are dropped. The allowlist applies to every effect
whose source is the remote actor itself, including its abilities and
equipment enchantments.

## Engine assumptions

Not confirmed in game:

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
   A remote actor's vanilla NPC graph holds an `attackStart` sent at `kHit`
   until `attackWinStart` instead of cutting the swing short, and an MCO-style
   graph rejects it outside its combo window and swings when it accepts it.
6. A power attack's attack state returns to `kNone` before the 4 s fallback, an
   actor without stamina still plays it, a power bash returns to the block, the
   forward power attack stands in acceptably for a sprinting one, and the clip's
   own motion does not fight the network translation during a lunge.
7. `MagicCaster::state` is `kNone` while a spell hand is readied and stays in
   `kUnk01..kUnk07` for the whole hold of concentration and fire-and-forget
   spells (`observed local hand presentation`), in `kUnk04..kUnk07` from a
   fire-and-forget release or a concentration loop's start until the cast
   ends, and the player's `IsDualCasting` stays true for a dual cast's hold.
8. `UnequipSpell` empties only the given hand and lowers it through
   `ActionForceEquip` (`WeapOutRightReplaceForceEquip`, `WeapSoloEquip`, or
   `Magic_Solo_Equip`). `DrawWeaponMagicHands(true)` readies spell hands without
   other blocked `Magic*` or `M?h_*` events.
9. The cast works from the left hand and while walking or running. A stand-in
   shows no beam or heal; if one appears, extend suppression to
   `MagicCaster::FindTargets`.
10. The graph takes `attackRelease` and locomotion events during a draw, the
    release clip ends within 0.5 s, `ArrowReleaseHandler` is the only launch path,
    and the release clip's own annotations still remove the nocked arrow.
11. The hand art (ID 38784 via `GetAVEffect`), light
    (`ActorMagicCaster::CheckAttachCastingArt`), and sounds
    (`ActorMagicCaster::PrepareSound`, ID 34438) come only from the selected
    spell's `GetAVEffect` (read from 1.7.104), so a stand-in shows the source's
    look, and swapping stand-ins on a drawn hand replaces the art and light.
12. The runtime stand-in forms stay valid across save loads, as runtime actor
    bases do ([Appearance and equipment](appearance-and-equipment.md#actor-base)).
13. Setting the high process `isDualCasting` and the caster's dual flag before
    `RequestCastImpl` plays the `DualMagic_*` charge, release, and loop clips with
    both hands' art while the right caster stays idle, as on the source.

Not confirmed in game for native hands ([Spells](#spells)):

14. `EquipSpell` equips a remote actor's own `SpellItem` in the requested hand
    slot as it does a stand-in, a ritual spell (both-hands equip slot) in both,
    and the caster then attaches its hand art and light.
15. A remote actor's native spell fire, with no combat target, aims an aimed or
    concentration spell along the actor's facing and launches its projectile
    or beam, applies a self spell to the actor, and finishes a fire-and-forget
    cast through `FindTargets`, so the cast state machine ends as with a
    stand-in. A concentration cast keeps firing at zero magicka, because
    `CheckCast` reports `kOK`.
16. `kSkipCheckCast`, set before `RequestCastImpl`, lasts until the cast's
    `FinishCastImpl` or `InterruptCastImpl` (IDs 34141, 34140) and makes the
    caster update (ID 34143) skip its magicka drain and charge interrupt
    (read from 1.7.104; the SE caster is assumed to match). No other magicka
    check outside `CheckCast` ends a remote cast.

Not confirmed in game for [beast actions](#beast-actions):

17. Every vocabulary event reaches the player's graph through the holder's
    vfunc 1, called virtually, including the attack data events, idles
    (feeding, howls, bats, casts), and casts; the string pool folds case, so
    `attackStartRight` and `AttackStartRight` are one pooled string.
18. A remote beast accepts each event at the time its source's graph did:
    attacks once its claws are drawn, howls and feeding from the stance the
    source used. A paired feeding clip plays alone without the victim.
19. A shader or art applied without a duration stays until marked finished,
    follows the actor, and `SetAlpha` holds while the engine updates the
    actor; neither passes the remote actor's magic firewall, which only
    stops active effects.

Not confirmed in game for [spell world isolation](#spell-world-isolation)
(each hook site was read from the 1.7.104 and 1.5.97 disassembly):

20. A remote actor's touch spell and every spell a cloak or hazard applies
    reach `MagicTarget::AddTarget` with the remote actor, or a reference whose
    actor cause is the remote actor, as `AddTargetData::caster`. A self spell
    reaches it with the caster as both caster and target.
21. Every spell, hit, impulse, and destruction a remote projectile or
    explosion causes passes through one of the hooked functions; in
    particular, a spell projectile's hit on an object reaches scripts
    (`OnHit`, `OnMagicEffectApply`) only through the spell impact (ID 34412)
    and `AddTarget`.
22. Every impact visual and impact-placed hazard of a projectile comes from
    the impact effects (ID 44213), and every explosion of a projectile from
    the projectile update's factory call (ID 44216 + 0x3C8), for missiles,
    lobbers, beams, flames, cones, and barriers. A hazard cast around the
    caster (`kSpawnHazard`), and the projectiles and explosions it launches,
    such as a blizzard's, carry the remote actor as `ownerActor` or actor
    cause.
23. The spell projectile launch (ID 34452) is the only path that launches a
    rune for a remote actor's cast, and returning true without a projectile
    ends a fire-and-forget target-location cast as a launch does.

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
   shows its art and light if it has those, and Healing Hands' look otherwise.
7. A cast plays the source spell's native clips in that hand: a
   fire-and-forget spell charges, holds ready for as long as the source holds
   it, and plays its release when the source releases; a charge the source
   cancels is cancelled; a concentration spell loops for as long as the source
   casts; self, aimed, ward, telekinesis, ritual, and dual casts play their own
   clips. A peer without the spell plays Healing Hands' cast, and no
   projectile, beam, effect, or release sound appears; a peer with it sees the
   spell's native release, self effects, and projectiles, beams, and sprays
   in flight, which vanish where they strike.
8. Sheathing, jumping, or sitting ends a draw, cast, or block first. A late
   joiner sees an ongoing draw, cast, or block.
9. Nothing is hit, staggered, damaged, pushed, or destroyed, and no local
   actor, object, or the local player receives an effect, including from a
   native hand's projectiles, beams, sprays, cloaks, and hazards. A native
   projectile leaves no explosion, impact effect, decal, or wall flames, and a
   rune is not launched. A native summon, reanimation, bound weapon, or
   scripted effect does nothing.
10. A werewolf or werebear peer's claw swings, power, side, running, and
    sprinting attacks, howls, and feeding play on its remote actor in order;
    a vampire lord peer's melee attacks, bats, feeding, and casts play too, and
    its actor hovers or stands as the source does, including for a late
    joiner. Its Mist Form shows the mist and a faded body for as long as the
    source's lasts, and its bats show the swarm for the dash.

## Known limitations

- A native hand's spell aims along the remote actor's facing, not the
  source's crosshair, so its projectiles and beams may strike elsewhere than
  on the source. Where it strikes shows nothing, and its hit art and shaders
  on local targets do not show, since its effects never attach there.
- A remote actor's self effects with an archetype outside the allowlist
  ([Spell world isolation](#spell-world-isolation)) do not show at all, and
  scripts attached to an allowed self effect still run on the remote actor.
- Non-goals: showing a peer's summons, reanimated thralls, or bound weapons.
  Each would be a local actor or item with its own synchronization.
- A native hand's light is the spell's own, not its
  [fake light](appearance-and-equipment.md#fake-lights) version, so a spell
  whose hand light casts shadows takes a shadow-casting light slot.
- Scrolls and a vampire lord's spells keep stand-ins: their casts show no
  effects.
- A peer without the spell plays Healing Hands' cast clips and sounds with the
  fallback art: the wire carries no cast shape.
- A fire-and-forget release that the receiver's charge cannot reach within
  `kReleaseChargeWaitSeconds` shows as a cancelled charge. A second
  fire-and-forget cast started so soon after a release that no checkpoint sees
  the hand idle or charging in between is not shown.
- A spell whose casting type or delivery differs between the source's and the
  receiver's plugins plays the receiver's version's clips.
- Crossbow attacks and bashes, shield charge (`BlockBashSprint`), block hit
  reactions, bash failures, sprint power attacks as their own animation (they
  start from idle records with perk conditions,
  `AttackRightForwardSprinting_2hand` and its children, which the action stream
  does not replicate), and creature attacks are not replicated. Kill moves
  show through [pose windows](remote-actor.md#source-window), not the action
  stream.
- Beast actions are animations only: a howl's fear or summon, Raise Dead's
  thrall, Vampiric Grip, the drain beam, and the feeding victim are not
  shown. A vampire lord's spell hands show the spell's hand look, not its
  casting type or delivery. Werewolf and vampire lord kill moves are not
  relayed.
- The source is invisible inside its bats (`DLC1BatsInvisEffect`); the remote
  vampire lord stays visible inside the swarm: fading the actor is expected to
  fade the bat art attached to its 3D as well. Mist Form's start and end sounds,
  image space, and end shader, and the bats' reform shaders, come from the
  effects' scripts and are not shown.
- A cancelled bow draw shows as a release.
- A replacer clip that swings again after a hit in a plain attack is captured
  as a chained attack, and a chained replacer swing that writes `kSwing`
  without a hit in between, with no pass seeing `kDraw`, is not captured.
- A graph that accepts an attack event and never swings it suppresses the
  capture of later accepted events until a swing; that swing is then captured
  once, so one attack can be missing.
- A receiver graph that accepts a chained event but drops it shows one swing
  fewer; the completion line reports it as `chainPending=true`.
- Stamina, perks, power attack slow motion, damage, and targeting are out of
  scope.
