# Locomotion

## Scope

Locomotion presents where a player is and how it moves: the transform, the
movement state, jumps, and swimming. It samples them on the source, and on the
receiver drives the actor's behavior graph and its direct translation.

It shares the graph with [Combat presentation](combat-presentation.md), which
owns weapon stance, attacks, and hand presentation, and hands the actor to
[Native interactions](native-interactions.md) while furniture, a mount, or
death owns it. Placement isolation is in
[Remote actor](remote-actor.md#world-isolation); checkpoint fields in
[Network](network.md#checkpoint-and-context).

The source's graph state is authoritative: the receiver never infers an
animation from speed or distance, and does not validate a transition against
its previous local state. Vanilla humanoid and beast graphs
([Beast graphs](#beast-graphs)) are the baseline; custom graphs may slide or
miss transitions.

## Components

| Module | Responsibility |
| --- | --- |
| `sync/movement/MovementSync` | Source transform sample; receiver transform target and coordinate-space checks |
| `sync/animation/AnimationSync` | Source movement state, jump, stance, graph epoch; receiver staging and the presentable state |
| `actor/ActorMovement` | Direct translation and heading, and the ground offset |
| `actor/GroundProbe` | The Havok ray that finds walkable ground below a point |
| `actor/ActorAnimation` | The actor's graph state and per-pass update: observation, landing, swim, locomotion edges and variables, retries, direction cross-blend, stance reconciliation, translation mode |
| `actor/WeaponTransition` | Draw and sheathe completion, the locomotion replay after them, the native draw or sheathe request, on foot and riding |
| `actor/GraphStance` | Sneak, standing/moving, and sprint selector states, the idle reading ([Idle presentation](idle-presentation.md#graph-reading)), and the sneak and `iState` variable reads |
| `actor/GraphProbe` | Debug builds: dump of active states, clips, and variables |
| `actor/BehaviorGraphLayout` | Havok layout reads: state names, current state, active behavior |
| `actor/JumpSound` | Distance attenuation of the actor's jump footstep sounds |

`ActorAnimation::State`, held by the `RemoteActor`, composes the dispatch,
landing, weapon transition, swim, stance reconciliation, attack
([`AttackDispatch`](combat-presentation.md#components)), hand presentation,
idle ([`IdlePresentation`](idle-presentation.md#components)), and direction
cross-blend parts of one graph instance, reset together
([Graph state](#graph-state)); graph recovery
([Remote actor](remote-actor.md#graph-recovery)) and the translation hold
outlive it. `ApplyAnimationUpdate` runs each pass, in order: graph
observation, landing, idle ownership, weapon transition, swim, the locomotion
dispatch (plan, stop edges, variables, start edges), direction cross-blend,
native draw or sheathe request, stance reconciliation, and the debug
[graph probe](#graph-probe). The landing sink check, the weapon transition,
and an idle release can end the pass early to give the graph an update. Attack
playback and graph recovery advance earlier in the actor update
([Remote actor](remote-actor.md#update-order)), hand presentation from
`CombatSync`.

## Source capture

At each checkpoint, from the carrier (the player, or its mount while riding):

- **Position** and **heading**: heading is the body angle, so backpedalling or
  strafing keeps facing the same way. Heading zero faces +Y.
- **Speed** of a moving checkpoint on foot is the player graph's `Speed`
  (`MovementSync::GetCheckpointSpeed`), so the receiver's legs play at the
  source's pace. The engine feeds it the intended speed divided by the actor's
  scale, not ground covered, so on stairs it keeps the full walk pace where a
  measured speed played too slowly. An idle checkpoint, a graph without
  `Speed`, and a mount carry the speed measured from the previous sample:
  horizontal, or 3D while swimming or flying
  ([Native interactions](native-interactions.md#dragon-flight)). Idle uses the
  measured value so a `Speed` still easing to zero does not defeat the server's
  idle suppression. A move of more than 4096 units is a relocation, not a
  speed sample.
- **Direction** is travel relative to heading in graph `Direction` units
  `[0, 1]` (0 forward, 0.25 right, 0.5 backward, 0.75 left), from the source
  graph's `Direction`, or from horizontal travel when a graph lacks it.
- **Movement state**: `idle`, `walking`, `running`, `sprinting`, and the three
  sneaking states, from `ActorState`. A change from moving to idle is published
  only once the movement bits have been clear for 0.06 s of clamped frame time
  (`ObserveLocalMovement`, every pass; `held local stop` while held), because
  direction-key changes clear the move input for 6–24 ms and chained attacks
  for about 6 ms, which peers would play as a stop and restart. Every other
  change is published at once.
- **Arrival hold**: after a load the player keeps the movement bits it had going
  through a door or fast travel until they clear by themselves. A sample
  without a comparable previous one (`LocalSample::relocated`: first sample,
  coordinate-space change, relocation) that reads a moving state publishes the
  standing state instead (`idle` or `sneakingIdle`) until a sample reads
  standing (`reason=bits-cleared`) or the carrier moved more than 8 horizontal
  units from where it arrived (`kArrivalMoveDistance`, `reason=moved`; the
  player drops onto the ground after arriving). Applied after the stop hold;
  its end logs `released arrival standing`.
- **Jumping**: `bInJumpState` gives the takeoff edge (physical jump or midair
  state when the graph lacks it). Once airborne, ground contact is the landing,
  since the flag stays set through landing cleanup; a stale true is then
  ignored until it resets. Leaving the ground again after a published landing
  is a new jump, since Skyrim accepts a takeoff from its landing branch. A
  takeoff cancelled before leaving the ground is released. No jump is
  published while swimming, where jumping only swims upward.
- **Swimming** from the carrier's `ActorState`.
- **Grounded**: the carrier's controller state (`context.currentState`) is
  `kOnGround` and it is not ragdolled (a ragdoll carries the reference off the
  ground while the controller still reports `kOnGround`), for the
  [ground offset](#ground-offset).
- **Graph epoch**: observed every pass, advanced when the graph manager is
  replaced or unavailable
  ([Combat presentation](combat-presentation.md#action-stream)).

## Receiver

### Coalescing

Locomotion and jump phase are current state. Movement state and jump phase
apply from the played checkpoint ([Sync pipeline](sync-pipeline.md#checkpoint-playout)),
the last one when several play in a pass; replaying skipped states would make
the graph traverse obsolete states. Position, heading, pitch, and the graph's
speed and direction follow the interpolated pose, so they never run ahead of
the movement state and do not step at 20 Hz. Takeoff and landing events are
derived only when the applied checkpoint changes jump phase.

### Graph state

- `Speed`, `SpeedSampled`, `SpeedDamped`, `Direction`, `TurnDelta`,
  `IsSprinting`, `iSyncSprintState`, and `iSyncIdleLocomotion` are written every
  pass, before translation. Speed and direction are the pose's `sampledSpeed`
  and `direction` (`ActorMovement::SetAnimationSample`), which the speed and
  direction channels also report
  ([Remote actor](remote-actor.md#graph-channels)). An invalid or zero sample
  while moving keeps the last `Speed` for `kZeroSpeedHoldSeconds` (0.25 s),
  since a held stop's last moving checkpoint can carry speed 0, then falls back
  to the actor's walk or run speed (zero speed for longer is running in place,
  where the source's graph plays its gait). Only `0_master.hkx` declares
  `SpeedDamped`, and its damping modifier damps another value, so writing it
  removes no damping.
- Compound changes are staged one graph update apart, so dependent events are
  not sent together: `idle -> running -> sprinting` and back; base movement
  before sprint; sneak movement after sneak idle; takeoff after grounded
  locomotion.
- Stop events are sent before clearing their variables and start events after
  setting them; `moveStop` and `SprintStop` are rejected after `Speed` is zeroed
  or `IsSprinting` cleared.
- A state is latched once its variables and events are accepted. A variable
  failure stays pending until the graph is writable. A rejected edge gets at
  least four attempts over at least 0.75 s, since Skyrim also returns false
  while an edge is active or transitioning and a power attack's exit refuses
  locomotion past 0.1 s; then convergence continues. An edge into the state the
  graph already holds is skipped, and a wrong stance left latched is repaired
  by [stance reconciliation](#stance-reconciliation).
- A graph that appears, is replaced, or becomes unavailable drops all dispatch
  tracking through `InvalidateAnimationDispatch`, as recovery, furniture exit,
  and mounting do: latched and staged locomotion, the landing, weapon
  transition, swim `moveStart` waits, stance reconciliation, attack, hand
  presentation, idle ownership with its applied instance, and the direction
  cross-blend. The next update asserts the current state in full. The cast
  ownership flag mirrors the registry's and is cleared by the next
  `ApplySpellCast`.
- `graph dispatch` lines report the result (`processed`,
  `retrying-stop-events`, `retrying-events`, `processed-event-timeout`,
  `variables-rejected`) and each axis's edge (`move`, `sneak`, `sprint`,
  `jump`): `-` not required, `pending`, `sent`, `skipped`, `implied` (a
  `SprintStart` entered through `iSyncSprintState` by `moveStart`), or
  `rejected`. `graph variable refresh changed` lists every write result when
  that set changes.

### Translation

- `ActorAnimation::GetTranslationMode` decides each pass, after the graph
  update and a deferred furniture entry, whether and how the actor translates,
  since translating while the graph request is unavailable or rejected is
  sliding:
  - it holds while the graph revision matching the checkpoint is unsettled, at
    most 0.15 s, then follows the pose with the graph state reached so far
    (holding for the whole retry left actors running on the spot, then jumping
    the backlog);
  - it follows the target state when that matches the presentable state
    (`AnimationSync::GetPresentableState`) or jumps, and the presentable state
    while a network attack plays;
  - it holds while the target is a staged step toward the presentable state, or
    while furniture owns the actor, except that a landing waiting for
    `JumpLandEnd` follows the target.
  The mode is airborne for a jumping state, attack while a network attack
  plays, otherwise moving or standing. While a
  [skeleton pose](remote-actor.md#receiver) has weight
  (`ActorMovement::State::poseWeight`, set by `SkeletonSync` the previous
  pass), the mode is posed, even where it would hold.
- A played checkpoint opens its revision in `ApplyAnimationUpdate`, just before
  the variable write that settles it (`RequestCheckpointUpdate`), not when it
  plays; otherwise every pass with a played checkpoint would read as unsettled
  and new states would wait for a pass without one (below 20 fps, until the
  source stopped). A pass ending with rejected variables or retrying edges
  leaves the revision open.
- Position approaches the pose (`ActorMovement::SetTargetTransform`) plus the
  [ground offset](#ground-offset) (`ActorMovement::GetPlacementTarget`) at the
  pose's path speed plus the remaining distance over about 0.5 s, so an actor on
  the path follows it exactly and one held behind catches up without a snap.
  `deltaTime` is clamped and a step never overshoots. While moving there is no
  arrival tolerance, since a slow walk at a high frame rate advances less than a
  unit per pass.
- Idle players drift (in recordings 31% of idle checkpoints moved 0.5–50 units,
  mostly about 20 units a second), so once the idle state settles an idle actor
  follows the pose like a moving one, draining a backlog over about 0.2 s rather
  than snapping in hops. It snaps only a backlog needing more than 150 units per
  second, and is left alone within the 1-unit stopping distance only while the
  pose is at rest (below 1 unit per second).
- Airborne, a bounded catch-up from the positional error over 0.2 s is added.
- While a network attack owns the graph, an idle checkpoint never snaps; the
  actor moves at the greater of the path speed and the remaining distance over
  0.2 s, since a power attack's lunge is clip motion the source publishes as
  slow `running` and then `idle`.
- Posed, the actor stands at the pose's position plus the ground offset every
  pass without a drain, and at full weight takes the pose's heading exactly,
  since the pose was sampled with that transform (any lag shows the animation
  beside its place, such as a climb off its wall). Below full weight the
  heading eases, since a heading snap starts `NPC_Turn` transitions that only a
  full pose hides. The blend-out keeps this placement.
- `SetPosition(next, true)` moves the reference and its controller. Heading
  follows the pose at its `headingRate` plus the remaining turn over 0.15 s, at
  most one turn per second, so a pose step (a dry playout, a batched
  checkpoint) is eased rather than snapped. Offsets within 0.5° are left alone
  only while the pose turns slower than 0.05 rad/s.

### Ground offset

Peers with different landscape mods have different ground heights, so an
actor at its source's exact height floats or sinks. The receiver keeps a
per-actor height offset (`ActorMovement::GroundOffset`) added to every network
placement and distance: translation, landing approach and alignment, furniture
entry alignment and exit check. A mount takes the same offset through its own
translation and carries its rider.

- **Probe.** While the played checkpoint says `grounded` and not `swimming`,
  `GroundProbe::FindSupport` casts a ray down at the checkpoint's position
  (not the pose's, which ahead of a takeoff is already rising) from 96 units
  above to 128 below, in the actor's cell's physics world, taking the first hit
  on a layer a character stands on. The target is the hit's height minus the
  checkpoint's; without a hit it is 0, since the source stands on something
  this world lacks, such as a modded bridge.
- **Edges.** A capsule rests on an edge its center has passed, so on a raised
  deck the center ray can fall to the floor below. When it misses or hits more
  than `kSupportTolerance` (8) below the checkpoint, 8 more rays at
  `kSupportRadius` (24) around it are cast and the hit nearest the checkpoint's
  height is taken. Where the local ground really is lower, the ring finds it
  lower too.
- **When.** A probe is due on the first pass, after 0.1 s once the source moved
  8 horizontal units since the last, and every 1 s regardless. After the first,
  it also waits until the source has been grounded for `kGroundSettleSeconds`
  (0.25 s), since a controller reports `kOnGround` as soon as its capsule
  catches a ledge, before the source climbs onto it. A probe starts only while
  fewer than `kMaximumGroundRaysPerPass` (24) rays were cast in the pass,
  counting edge rings (reset by `ActorMovement::BeginPass`); an actor over the
  budget stays due for a later pass.
- **Hold.** While the source is off the ground and until it settles, there is
  no probe and the target holds, so jumps keep their arc. A played checkpoint
  with a [skeleton pose](remote-actor.md#receiver) holds it too, since a climb
  keeps its controller grounded while rising. Climbs start at a wall and end at
  a lip, where edge rings misread the ground, so when the first posed
  checkpoint plays less than `kPosedEdgeLeadSeconds` (0.5 s) after edge rings
  began giving the target, the target returns to the last center-ray or miss
  value (`Remote actor ground offset dropped edge probes before a pose`), and
  after the posed run the first probe waits `kPosedGroundSettleSeconds` (0.5 s).
  While swimming the target is 0, since water levels match between load orders
  far more often than ground.
- **Smoothing.** The offset eases toward its target with a 0.15 s time
  constant; the first probe sets it at once. A checkpoint more than 256 units
  from the previous one clears the offset and its probe state.
- `Remote actor ground offset target changed` logs the first probe, a change
  between hit and miss, and a target 16 units or more from the last logged,
  with the hit layer and `edge`.

### Direction

Vanilla locomotion selects forward, backward, and strafe clips only through
`Direction` (`BSCyclicBlendTransitionGenerator.fBlendParameter` over
`*_Direction_Blend`); there is no backward event. Skyrim copies an actor's own
movement direction into `Direction` through `ActorDirectionChannel` after
main-loop writes, always forward for a remote actor, so the channel reports the
network direction instead. As Skyrim does for the player, a change of at least
1/8 turn is cross-blended with `CyclicFreeze`, then `CyclicCrossBlend` one graph
update later, at most once per 0.25 s so the 0.2 s blend is not restarted
while the source's `Direction` ramps. Smaller changes interpolate with the
pose; larger ones step at the played checkpoint under the cross-blend.

- `Direction` keeps the network value while standing, since the source's graph
  keeps its last `Direction` when it stops and the directional blend generator
  stays active while standing. A start in another direction is therefore
  cross-blended like a change while moving.
- The first move after a spawn, a jump, or a graph invalidation only records
  its direction.

### Turning

Remote actors never turn in place; heading comes from the network. Their AI
still asks to face a package or combat target, so Skyrim sends `TurnLeft`,
`TurnRight`, `turnStop`, and `NPC_Turn*` and records the request in
`iSyncTurnState`. The firewall blocks the events, but every vanilla idle turn
machine (`MT_Idle_TurnBehavior`, `1HM_Idle_Behavior`, `Bow_Idle_Behavior`,
`MagicReadyIdleTurnBehavior`, ...) binds its start state to `iSyncTurnState`
(0 right, 1 standing, 2 left), and would enter a turn clip it leaves only on
the blocked `turnStop`. So `iSyncTurnState` is held at 1 before each graph
update, with a network `turnStop` whenever the AI changed it, and
`ActorTurnDeltaChannel` reports 0.

### Standing re-entry

Every vanilla standing/moving machine (the base locomotion of each stance,
`Swim_Behavior`, `Torch_Ready_Behavior`, the block, bow, cast, equip, and
unequip lower bodies) binds its start state to `iSyncIdleLocomotion` (0
standing, 1 moving). No vanilla node writes it, so on a remote actor every such
machine entered while moving would start standing. The receiver writes it
every pass: 1 while the dispatched state moves, 0 otherwise. A graph without
the variable shows `syncIdle=false` in `graph variable refresh changed`.

- With it, no one-shot `moveStart` follows a sneak edge, a block, bow draw, or
  cast started while moving, or a weapon transition; captures showed such
  repairs rejected or redundant every time.
- `Swim_Behavior` keeps its `moveStart` after `SwimStart`
  ([Swimming](#swimming)).
- The locomotion, sneak, jump, and landing machines start in the sprint or
  non-sprint state stored in `iSyncSprintState`, which a sprinting takeoff
  leaves at sprint, so the receiver writes it from the replicated state.
- When a landing completes, an attack, bow shot, cast, or block ends, or
  swimming stops (`locomotionReassertReason`: `landing`, `attack`, `bow-shot`,
  `cast`, `block`, `swim-stop`), the next dispatch asserts the target's
  locomotion edges once no attack or [idle](idle-presentation.md#ownership)
  plays and the actor no longer swims; edges the graph holds are skipped. A
  pending reassert counts as owning the graph for hand presentation starts,
  attack dispatch, and stance reconciliation. Logs `reasserted locomotion`.
- Other standing re-entries, such as a force equip's standing branch, are
  repaired by [stance reconciliation](#stance-reconciliation).

### Stance reconciliation

A sneak or move edge that ran out of retries is latched whether or not the
graph took it, and a branch entered standing leaves the graph standing under a
moving latch, so the graph could keep a wrong stance (crouched while the
source walks) until the source changes state again.

- `GraphStance::ReadStanceSelectors` reads three axes in one walk of the active
  nodes. In the vanilla third-person behaviors every `SneakStart`, `SneakStop`,
  `moveStart`, `moveStop`, `SprintStart`, and `SprintStop` transition belongs
  to a two-state selector: default or sneak variant
  (`MT_LocomotionType_Behavior`, `Melee_DirectionType_Behavior`,
  `Weap_IdleType_Behavior`, `BowLoweredType_Behavior`,
  `MagicReadyRootBehavior`, equip and block selectors, ...), standing or moving
  (the machines of [Standing re-entry](#standing-re-entry)), or locomotion or
  sprint (`MT_Locomotion_Behavior`, `Sneak_Locomotion_Behavior`,
  `Melee_Locomotion_Behavior`, `MagicReadyLocomotion_Behavior`, ...). The
  current state switches when the event is taken, while the old branch blends
  out. Turn, shout, first-person, horse, flinch, stagger, bleedout, drunk, and
  crossbow selectors are left out, as are the magic speed samplers (re-enter
  idle inside a moving branch), the jump and landing sprint variants (linger
  through a landing), and machines that choose only on entry. Each axis reads
  on or off when every active selector agrees, mixed when they disagree, none
  when none is active.
- `GraphStance::ReadSneak` falls back to variables when no sneak selector is
  active: sneaking when `IsSneaking` is true or `iState` is 2, not sneaking
  only when both are readable and neither holds. The vanilla sneak branches set
  `IsSneaking` (`BSIsActiveModifier`) or tag `iState` 2
  (`BSiStateTaggingGenerator`), but not all both and transitional branches
  neither, and both stay set while a sneak branch blends out, so a
  not-sneaking variable reading counts only while nothing else owns the graph.
  Standing, moving, and sprinting have no variable fallback (`bInMoveState` and
  `iState` do not tell them apart in every stance, [Graph probe](#graph-probe)),
  so without a selector their edges are always sent.
- The dispatch reads the selectors once before sending any event of the pass
  and skips an edge into the state the graph holds, since Skyrim rejects it
  for the whole retry window: `moveStop` standing, `moveStart` moving,
  `SprintStop` not sprinting, `SprintStart` sprinting, `sneakStop` when the
  selectors read not sneaking (never from variables alone), `sneakStart` when
  sneaking by selectors or variables. A skipped `moveStart` does not count as
  entering `Sprint_State` for a rejected `SprintStart`.
- `ReconcileGraphStance` samples every 0.1 s while the dispatch is latched on
  the target and nothing else owns the graph: grounded, not sprinting, no
  attack or reassert pending, no unsettled weapon transition, no bow draw,
  cast, block, or idle.
  - A sneak disagreement held 0.5 s, longer than a `sneakStop` blend through
    the variable fallback, re-latches the opposite sneak variant
    (`ToggleSneak`).
  - Standing selectors under a moving target for 0.2 s (two samples) re-latch
    the standing variant (`ToStanding`). Moving selectors under an idle target
    are left alone, since `moveStop` is rejected once `Speed` is zeroed.
  - The next dispatch sends the missing edge and retries, at most three times
    per axis and target state, so a graph reporting stance differently cannot
    keep sending edges.
- Dispatch logs carry the pre-event `graphMoving` (`standing`, `moving`,
  `mixed`, `unknown`) and `graphSneak` with its source (`sneaking/states`,
  `notSneaking/states`, `mixed/states`, `sneaking/variables`,
  `notSneaking/variables`, `unknown`), `unread` when no such edge was required.
  Corrections log `graph sneak disagrees with the latched state` or `graph
  stands while the latched state moves`; a mixed reading logs `stance selectors
  disagree` with every active selector, on the first and every 60th retry.

### Beast graphs

A [beast form](appearance-and-equipment.md#shapeshifts) plays
`WerewolfBehavior.hkx` (also the werebear's) or `VampireLord.hkx`, whose
locomotion inputs match the human ones (`moveStart`/`moveStop`,
`sprintStart`/`sprintStop`, jump and landing events, `SwimStart`/`SwimStop`,
`weapEquip`/`weapUnequip`; the string pool folds case), so the same dispatch
drives them.

- Neither graph has `SneakStart`: `SetAnimationState` maps a beast's sneaking
  target to its walking, running, or idle variant.
- `GraphStance` also lists the beast selectors: standing or moving in
  `MovementBehavior` (both), `Behavior03` and the swim machine `Behavior06`
  (werewolf), `CombatReadyBehavior`, `EquipBehavior`, `UnequipBehavior`,
  `LevitationMovementBehavior`, `LevitateCombatBehavior`, and
  `CastState_Ready_Behavior` (vampire lord); sprinting in `H2HBehavior`
  (werewolf), `Ground_StateBehavior`, and `Levitation_Behavior` (vampire lord).
  A fourth axis reads the vampire lord's levitation from its `H2HBehavior`
  (`Ground_State_Cam`, `Levitate_End` off; `LevitationBase_State`,
  `Levitate_Start` on), so a toggle counts as soon as it is taken
  ([Combat presentation](combat-presentation.md#levitation)). A same-named
  machine of another graph matches none of them.
- A beast's draw and sheathe have no clip exit annotation, so the transition
  completes when Skyrim's weapon state reaches drawn or sheathed, or at the
  2.5 s bound; `IdleForceDefaultState`, which the beast graphs lack, is never
  sent.
- The idle turn machines number states as the human ones do, so the
  [idle turn hold](#turning) applies unchanged.

### Graph probe

A debug-build diagnostic (without `NDEBUG`, the XMake `debug` mode) for telling
standing from moving in every stance; each probe builds several kilobytes
under the graph lock and keeps a copy of the variables per actor, which does not
scale. No variable tells them apart (`bInMoveState` is set only in weapon and
bow locomotion and sprint; `iState` reads 0 for both unarmed states and 9 for
both magic states), but the active state names one machine below the stance
root do, which the stance selectors use.

- `Animation graph probe` lines carry the latched state, `iState`, and
  `GraphProbe::Describe`: every active `hkbStateMachine` with its current
  state, every active `hkbClipGenerator`'s animation, and the root graph's
  word-sized variables changed since the previous probe (all on a new graph).
- `reason=state` is logged once the latched state changed and the dispatch
  stayed settled 0.5 s with no attack, hand presentation, unsettled weapon
  transition, or recovery; `reason=reassert` and `reason=weapon-replay` the
  same way after a reassert or a weapon transition's replay;
  `reason=unsettled-translation` follows the `resumed translation while its
  locomotion dispatch is unsettled` line at once.
- It reads `hkbBehaviorGraph::activeNodes` and `variableValueSet` under the
  manager's `updateLock`, with two Havok 2010 layouts CommonLib leaves unnamed:
  the value set's word array right after `hkReferencedObject`, and a state
  info's name at 0x60 and ID at 0x68. The stance read walks the same nodes and
  recognizes a state machine by an exact `VTABLE_hkbStateMachine` match instead
  of an RTTI cast per node.

### Jumps

- Takeoff restores `bInJumpState` and the jump sample's variables, then sends
  `JumpStandingStart` or `JumpDirectionalStart`.
- Landing waits for the airborne actor to reach the landing checkpoint's
  position (`ActorMovement::State::checkpointPosition` plus the
  [ground offset](#ground-offset)), at most 0.25 s (then it lands on the pose),
  aligns it there, restores `bInJumpState=false` and the locomotion variables,
  and only then sends `JumpLand`.
- The applied jump value is reasserted immediately before and after the actor
  update, so the locally grounded controller cannot overwrite it.
- A graph output-event sink watches for the `JumpLandEnd` annotation. The
  landing completes when it is seen, or after a 2 s fallback that sends
  `JumpLandEnd` itself; every active grounded axis is then reasserted, so the
  actor cannot leave the branch in a sliding idle pose. The dispatch history is
  kept, since forgetting it would read a drawn weapon as a new draw. Translation
  continues meanwhile; locomotion edges are held, since the jump branch rejects
  them.
- No jump starts while the actor swims: interpolation can put it at swim depth
  while its source still wades.

### Jump sounds

The vanilla jump states emit `JumpUp` and `JumpDown`, which Skyrim plays as
footstep sounds of the actor's set (`DefaultFootstepJumpUpImpact` 0x5A288,
`DefaultFootstepJumpDownImpact` 0x12F0E) by ground material, through a shared
handle pool for non-player actors. Every takeoff and the stone-like landing
sounds use `SOMMono01400Player1st` (0xB4058), a fixed-speaker output model that
does not attenuate, so every remote jump would be heard at full volume anywhere.

`JumpSound` fixes this for remote actors only:

- The footstep sink (ID 36209) marks a registered actor's `JumpUp` or `JumpDown`
  for the dispatching thread; other footsteps pay one pooled-string comparison.
- Its call to `PlayImpactDataSounds` (ID 36209 + 0x53D) scales the volume of a
  sound whose `BGSStandardSoundDef` output model lacks
  `kAttenuatesWithDistance` by the distance from `BSAudio::audioListener`,
  with `SOMMono01800`'s curve (vanilla NPC landings on soft ground): full to
  150 units, then 100, 50, 20, 5, and 0 % evenly to 1800 units; beyond, the
  sound is not played.
- The pool's call to `BSSoundHandle::Play` (ID 36208 + 0x182) sets that volume
  just before play.

The play hook is installed first and the others only if it succeeded, so a jump
is never routed without its volume.

### Swimming

Skyrim manages the swim transitions locally; the firewall admits `SwimStart`
and `swimStop` ([Remote actor](remote-actor.md#swimming) keeps controller
support consistent with the source).

- `Swim_Behavior` enters `SwimIdle` or `SwimLocomotion` from
  `iSyncIdleLocomotion` and leaves `SwimIdle` only on `moveStart`. When
  `SwimStart` is accepted while dispatched and target locomotion both move, the
  pass waits two main-loop passes for the swim branch, then sends `moveStart`
  unless the selectors already read moving, until accepted, the actor stops or
  leaves the water, or 0.5 s pass.
- After `swimStop` is accepted and the actor leaves the swimming state, the
  locomotion events are reasserted.
- `SwimStart`, `JumpLand`, and `JumpLandEnd` are wildcard transitions of the
  root machine, and `JumpLandEnd` leads to land locomotion from `Swim_State`
  too. A jump into water arrives as a landing at the surface followed by
  `SwimStart`, so the landing clip never emits `JumpLandEnd`. While the actor
  swims, a pending landing ends without its fallback or reassert, and a landing
  arriving after `SwimStart` sends no `JumpLand`; otherwise the actor would walk
  on the riverbed.

## Engine assumptions

Read from the 1.7.104 executable, `Skyrim.esm`, and the vanilla behavior files:

1. The footstep sink plays a non-player actor's impact sound through
   `ImpactSoundData::pool` and its first sound only.
2. `BSSoundHandle::SetVolume` before `Play` scales a non-attenuating sound, and
   reaches the audio thread before the play message.
3. An output model's five curve values are evenly spaced from its minimum to
   maximum distance.
4. A `BSIsActiveModifier` output returns to its inverted value when its branch
   deactivates, so a true `IsSneaking` means a sneak branch is active.
5. A stance selector's `currentStateID` changes as soon as the graph takes the
   transition, before the blend completes, and a sneak selector entered later
   starts in the variant matching the actor's stance.
6. Every `hkbStateMachine` in `activeNodes` has exactly the `hkbStateMachine`
   vtable.
7. A state machine reads its bound `startStateId` on entry, and nothing in the
   engine writes `iSyncIdleLocomotion` for an unpaired actor (its only reference
   by name is the `ActorCopyGraphVariableChannel<int>` built in ID 40358, which
   takes two actors).
8. The beast graphs take the human locomotion inputs as the human graph does,
   and a beast's weapon state reaches drawn and sheathed after a native draw
   and sheathe.
9. A player holding a direction key through a loading screen keeps its movement
   bits and moves once control returns, which the
   [arrival hold](#source-capture) releases by distance.
10. `bhkWorld::PickObject` (vfunc 0x33, ID 77860) takes the world's read lock
    (ID 68233 on `bhkWorld + 0xC598`) when Havok runs multithreaded, casts with
    `bhkPickData::closestRayHitCollector` through `hkpWorld::castRay`
    (ID 61400), and copies the hit when `rootCollidable` is set; a non-zero
    `bhkPickData::ray` replaces `to` with `from + ray`.
11. The ray's layer `L_CHARCONTROLLER` (30) meets what a character stands on:
    per the COLL records it collides with every layer `GroundProbe` accepts
    (static, animated static, transparent, clutter, trees, props, terrain, trap,
    ground, large debris, the two small transparent layers, invisible walls,
    stair helpers) and is the only layer colliding with `L_STAIRHELPER` (31).
    The collector skips water, triggers, characters, and other layers without
    touching its early-out fraction. A remote actor's own body has the
    no-collision bit ([Remote actor](remote-actor.md#character-body)).
12. A hit's fraction is along the whole ray, and a horse's rigid-body controller
    reports `kOnGround` on the ground as the player's proxy controller does,
    briefly leaving it on steps and slopes.
13. The graph probe's value-set and state-info offsets come from the Havok 2010
    class layouts, unchecked against the executable.

## Acceptance criteria

1. A remote actor follows its source's path, walking, running, sprinting, and
   sneaking with the source's clips and speed, without sliding in an idle pose.
2. Backpedalling and strafing play directional clips while facing the source's
   heading; the actor never turns in place.
3. Standing, directional, and repeated jumps take off and land where the source
   did and return to the right gait, also after a sprinting jump.
4. Swimming, leaving the water, and jumping into water play swim and land
   locomotion without walking on the riverbed.
5. Dropped intermediate checkpoints converge to the newest state.
6. With a 20 Hz server tick, a moving actor keeps an even pace along its
   source's path without periodic stops or surges, at 30 and 144 fps.
7. A remote jump is heard near the actor, fades with distance, and is silent
   beyond about 1800 units on any ground; the local player's jump sounds are
   unchanged.
8. A remote actor that draws while sneaking and then stands up is not left
   crouching, and one that sneaks during a draw does not stay standing.
9. A remote werewolf, werebear, or vampire lord walks, runs, sprints, jumps, and
   swims with its own clips without sliding and draws and sheathes its claws; a
   vampire lord also while hovering.
10. A player who runs through a door or fast travels appears standing in the
    new space until it moves there.
11. Where the peers' ground differs by up to about 96 units up or 128 down, a
    remote actor or mount walks, stands, and lands on the local ground, and its
    jumps keep their arc.

## Known limitations

- Head look independent of the body is not replicated; aim pitch is
  ([Combat presentation](combat-presentation.md#aim)).
- Jitter beyond the playout delay leaves nothing to interpolate toward: the
  actor stops at the played checkpoint and catches up once the next one plays.
- A graph mismatch between peers is not detected; missing variables and
  rejected or timed-out events are logged.
- Stance selectors are recognized by vanilla machine and state names. A graph
  that renames them falls back to the sneak variables, with their lag after
  `SneakStop`, and without `IsSneaking` or `iState` 2 a timed-out sneak edge
  stays latched; it gets no move skips or standing correction.
- A graph left moving under an idle target is not corrected.
- The hold for a staged step has no bound: an intermediate step the graph keeps
  rejecting holds translation for the locomotion retry window, up to 0.75 s.
- An attenuated jump sound keeps its fixed-speaker output: it fades but has no
  direction.
- The [ground offset](#ground-offset) leaves a source standing on something
  missing locally at its own height rather than on the ground below (a riverbed
  under a bridge, the inside of a building), and does not correct differences
  beyond the probe window. Clutter counts as ground where it lies locally.
- The offset is sampled at most every 0.1 s and eased, so on ground whose
  difference changes quickly the feet lag slightly.
