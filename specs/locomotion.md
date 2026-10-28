# Locomotion

## Scope

Locomotion presents where a player is and how it moves: the transform, the
movement state, jumps, and swimming. It samples them on the source, and on the
receiver it drives the actor's behavior graph and its direct translation.

It shares the graph with [Combat presentation](combat-presentation.md), which
owns weapon stance, attacks, and hand presentation, and hands the actor to
[Native interactions](native-interactions.md) while furniture, a mount, or
death owns it. Placement isolation is in
[Remote actor](remote-actor.md#world-isolation). The checkpoint fields are in
[Network](network.md#checkpoint-and-context).

The source's own graph state is authoritative: the receiver never infers an
animation from speed or distance, and does not validate a transition against
its previous local state. Vanilla humanoid graphs are the baseline, and the
vanilla beast graphs are driven the same way ([Beast graphs](#beast-graphs));
custom graphs may slide or miss transitions.

## Components

| Module | Responsibility |
| --- | --- |
| `sync/movement/MovementSync` | Source transform sample; receiver transform target and coordinate-space checks |
| `sync/animation/AnimationSync` | Source movement state, jump, stance, and graph epoch; receiver staging and the translation gate |
| `actor/ActorMovement` | Direct translation and heading on the actor, and its ground offset |
| `actor/GroundProbe` | The Havok ray that finds the walkable ground below a point |
| `actor/ActorAnimation` | The actor's graph state machine: variables, events, staging, retries, landing, swim, weapon and presentation transitions, recovery |
| `actor/GraphStance` | The sneak, standing/moving, and sprint selector states of the actor's active behavior |
| `actor/GraphProbe` | Debug builds only: the diagnostic dump of active states, clips, and variables |
| `actor/BehaviorGraphLayout` | Havok layout reads shared by both: state names, the current state, the active behavior |
| `actor/JumpSound` | Distance attenuation of the actor's takeoff and landing footstep sounds |

`ActorAnimation` is one controller on purpose: landing, draw and sheathe, swim,
recovery, and attack transitions share one graph lifecycle.

## Source capture

At each checkpoint, from the carrier (the player, or its mount while riding):

- **Position** and **heading**. Heading is the body angle, so a backpedalling or
  strafing player keeps facing the same way. Skyrim heading zero faces +Y.
- **Speed** of a moving checkpoint on foot is the player graph's `Speed`
  (`MovementSync::GetCheckpointSpeed`), so the receiver's legs play at the
  source's pace. The engine's Speed channel feeds that variable the intended
  movement speed divided by the actor's scale, not the ground covered: on
  stairs the source keeps its full walk pace while its controller covers less
  ground, and measured speed played the remote's walk too slowly there
  (not confirmed for scaled races). An
  idle checkpoint, a graph without `Speed`, and a mount carry the speed
  measured from the previous published sample instead: horizontal travel
  only, or 3D travel while swimming, so a mount's gait follows its ground
  speed. Idle keeps the measured value so a graph `Speed` still easing to zero
  does not defeat the server's idle repeat suppression. A jump of more than
  4096 units is a relocation, not a speed sample.
- **Direction** is travel relative to heading in graph `Direction` units
  `[0, 1]` (0 forward, 0.25 right, 0.5 backward, 0.75 left), read from the
  source graph's `Direction`, or derived from horizontal travel when a custom
  graph lacks it.
- **Movement state**: `idle`, `walking`, `running`, `sprinting`, and the three
  sneaking states, from the player's `ActorState`. A change from moving to
  idle is published only once the movement bits have been clear for 0.06 s of
  clamped frame time (`ObserveLocalMovementInput`, every pass); until then the
  previous moving state is published and `held local stop` is logged.
  Direction-key changes left the move input zero for 6–24 ms, and chained
  attacks dropped the bits for about 6 ms with the input held, so a 20 Hz
  sample inside such a gap published a stop that peers played as a stop and
  restart. A genuine stop reaches peers up to one checkpoint later; starts and
  every other change are published at once.
- **Arrival hold**: after a load the player keeps the movement bits it had
  going through the door or fast travel until they clear by themselves, so
  peers saw it run in place in the new space for seconds. A sample without a
  comparable previous one (`LocalSample::relocated`: the first sample, a
  coordinate-space change, or a relocation) that reads a moving state starts
  a hold that publishes the standing state instead (`idle`, or `sneakingIdle`
  while sneaking). It ends when a sample reads a standing state (the bits
  cleared, `reason=bits-cleared`), or when the carrier has moved more than 8
  horizontal units from where it arrived (`kArrivalMoveDistance`,
  `reason=moved`), which releases a player who kept moving through the load
  and whose bits never clear. The distance is horizontal because the player
  drops onto the ground after arriving. The hold is applied after the stop
  hold and logged as `holding arrival standing` and `released arrival
  standing`.
- **Jumping**: the graph's `bInJumpState` gives the responsive takeoff edge
  (physical jump or midair state when the graph lacks it). Once the player has
  actually been airborne, physical ground contact is the landing, since the
  graph flag stays set during landing cleanup; a stale true value is then
  ignored until it resets once. Leaving the ground again after a published
  landing is a new jump, because Skyrim accepts a takeoff from its landing
  branch and `bInJumpState` may never reset between jumps. A takeoff cancelled
  before leaving the ground is released. Swimming has no takeoff: jumping only
  swims upward, so no jump is published while swimming.
- **Swimming** from the carrier's `ActorState`.
- **Grounded**: the carrier's character controller state
  (`context.currentState`) is `kOnGround`, for the
  [ground offset](#ground-offset).
- **Graph epoch**: observed every pass, and advanced when the player's graph
  manager is replaced or becomes unavailable
  ([Combat presentation](combat-presentation.md#action-stream)).
- **Movement input diagnostic**: `ObserveLocalMovementInput` also keeps the last 12
  passes of the player's `ActorState` movement, run, sprint, and sneak bits,
  `PlayerControls` move input, auto-move and run flags, whether movement
  controls are enabled, weapon and attack state, and midair. When a published
  sample stops moving, and on the sample after it, `local movement input`
  logs them newest first with each pass's age (`reason=stopped` or
  `after-stop`), which shows why a sample published a stop between moving
  ones.

## Receiver

### Coalescing

Locomotion and jump phase are current state, not action history. Checkpoints
play at their sampled spacing ([Sync pipeline](sync-pipeline.md#checkpoint-playout)),
and movement state and jump phase are applied from the played checkpoint;
when several play in one pass, only the last is applied. Position, heading,
pitch, and the graph's speed and direction follow the pose interpolated from
the played checkpoint toward the next, so they never run ahead of the
movement state sampled with them. Applied from the played checkpoint, speed
and direction would move the walk/run and directional blends in 20 Hz steps
while the position moved smoothly. Replaying skipped states would make the
graph traverse obsolete states, and pairing a newer transform with an older
locomotion state slides the actor. Takeoff and landing events are derived only
when the applied checkpoint changes jump phase; no jump events are queued.

### Graph state

- Variables `Speed`, `SpeedSampled`, `SpeedDamped`, `Direction`, `TurnDelta`,
  `IsSprinting`, `iSyncSprintState`, and `iSyncIdleLocomotion` are refreshed
  every pass, in the same pass as translation and before it. Speed and
  direction are the pose's `sampledSpeed` and `direction`
  (`ActorMovement::SetAnimationSample`, every pass), which the speed and
  direction channels report too ([Remote actor](remote-actor.md#graph-channels)).
  An invalid or zero sample while moving keeps the last written `Speed` for
  `kZeroSpeedHoldSeconds` (0.25 s), then falls back to the actor's own walk or
  run speed. A [held stop](#source-capture)'s last moving checkpoint can carry
  speed 0 (measured speed did in 15 of the stops in the server recordings, 5
  of about 40 short taps in one), and an immediate fallback would push `Speed` to the
  full run speed just before `moveStop`.
  Zero speed lasting longer is running in place, against an obstacle or
  through a load, where the source's graph plays its gait. In the vanilla
  character behaviors only `0_master.hkx`
  declares `SpeedDamped`, its one `hkbDampingModifier` damps another value,
  and the executable never names it, so writing it removes no graph damping.
- Compound changes are staged one graph update apart, so dependent events are
  not sent together: `idle -> running -> sprinting` and back; base movement
  before sprint; sneak movement after sneak idle; takeoff after grounded
  locomotion.
- Stop events are sent before clearing their old variables, and start events
  after setting the new ones. `moveStop` and `SprintStop` were rejected when sent
  after `Speed` was zeroed or `IsSprinting` cleared.
- A state is latched once its variables and required events are accepted. A
  variable failure stays pending until the graph is writable. A rejected event
  gets four attempts spread over at least 0.1 s, so a high frame rate cannot
  spend them inside one graph blend, then convergence continues: Skyrim also
  returns false while an edge is already active or transitioning, so the result
  is not a reliable acknowledgement. Locomotion edges retry for 0.75 s instead:
  the graph refuses locomotion while it is still leaving an attack, and a power
  attack's exit was measured holding it past 0.1 s. An edge into the state the
  graph already holds is skipped, and a latched sneak edge the graph never took
  or a standing graph under a moving latch is repaired by [stance
  reconciliation](#stance-reconciliation).
- A graph that first appears, is replaced, or becomes unavailable (no 3D or no
  graph manager) drops all dispatch tracking through
  `InvalidateAnimationDispatch`, as recovery, furniture exit, and mounting do:
  latched and staged locomotion, the landing and weapon-transition waits,
  attack and hand-presentation ownership, and the direction cross-blend. The
  next update asserts the current state in full.
- Logs distinguish `processed`, `retrying-events`, `processed-event-timeout`, and
  `variables-rejected`.

### Translation

- Translation waits for the graph revision that matches the checkpoint and for
  staging to reach it, since translating while the graph request is unavailable
  or rejected is the literal sliding failure. The hold gives up after 0.15 s
  and follows the pose with the graph state reached so far: a rejected
  locomotion event retries far longer than an actor may visibly stand still,
  and holding for the whole retry left it running on the spot and then jumping
  the backlog.
- A played checkpoint opens its revision in `ApplyAnimationUpdate`, just
  before the variable write that settles it (`RequestCheckpointUpdate`). If
  the revision opened when the checkpoint played, the state staging, attack,
  hand-presentation, and furniture-entry checks earlier in the pass would
  read every such pass as unsettled. Each new state, attack, or presentation
  would then wait for a pass without a checkpoint, and below 20 fps it would
  wait until the source stopped. A pass that ends with rejected variables or
  retrying edges leaves the revision open, so the next pass's checks still
  wait.
- Position approaches the pose (`ActorMovement::SetTargetTransform`, every
  pass), raised or lowered by the [ground offset](#ground-offset)
  (`ActorMovement::GetPlacementTarget`), at the pose's path speed plus a backlog drain of the remaining distance
  over about 0.5 s, so an actor on the path follows it exactly and one held
  behind it, by a translation hold or a pause in arrivals, catches up without
  a snap. `deltaTime` is clamped and a step never overshoots the pose. While
  moving there is no arrival tolerance: at a slow walk and a high frame rate
  the pose advances less than one unit per pass, and a tolerance made the
  actor move only every other pass.
- Idle players still move: in the recordings, 31% of idle checkpoints moved
  0.5–50 units, mostly a horizontal drift of about 20 units per second, and the
  first idle checkpoint after a stop carries the deceleration (median 2 units,
  up to 22). Snapping to the target once it is more than 2 units away (the
  1-unit stopping distance and tolerance) would move such an actor in hops of
  2–3 units every ~0.1 s. Once the idle graph state settles, an idle actor instead
  follows the pose like a moving one, draining a backlog over about 0.2 s. It
  snaps only a backlog that would need more than 150 units per second to
  drain, since sliding that far in an idle pose shows more than a jump. It is
  left alone within the stopping distance and tolerance only while the pose
  is at rest (below 1 unit per second).
- Airborne, the rate also derives a bounded catch-up from the positional error
  over 0.2 s.
- While a network attack owns the graph, an idle checkpoint never snaps; the
  actor moves at the greater of the path speed and the remaining distance
  over 0.2 s. A forward power attack's lunge is clip motion, so the source's
  checkpoints say `running` at a low speed while it accelerates and `idle` while
  it still carries forward; without this the actor falls behind and teleports
  on the idle checkpoint.
- `SetPosition(next, true)` moves the reference and its controller. Heading
  follows the pose like position: at the pose's `headingRate` plus the
  remaining turn over 0.15 s, at most one turn per second, so an actor on the
  pose turns with it exactly and a step in the pose is eased. Offsets within
  0.5° are left alone only while the pose turns slower than 0.05 rad/s.
  Turning toward the pose at a fixed one turn per second would close a 20 Hz
  step within a frame or two, so a turn whose pose stepped (a dry playout, or
  one checkpoint batched over two) would snap at the checkpoint rate.

### Ground offset

Peers with different landscape mods have different ground heights at the same
place, so an actor placed at its source's exact height floats or sinks. The
receiver keeps a per-actor height offset (`ActorMovement::GroundOffset`) and
adds it to every network placement and every distance to one: translation,
the landing approach and alignment, and the furniture entry alignment and exit
check. The mount takes the same offset
through its own translation, and its rider is carried with it.

- **Probe.** While the checkpoint says `grounded` and not `swimming`,
  `GroundProbe::FindGround` casts a ray straight down at the source's position
  (the pose, or the pending landing position) from 96 units above it to 128
  below, in the actor's cell's physics world, and takes the first hit on a
  walkable layer. The offset target is the hit's height minus the source's;
  without a hit it is 0, since the source then stands on something this world
  lacks, such as a modded bridge, and its own height is the better guess than
  the ground under it.
- **When.** A probe is due on the first pass, once 0.1 s have passed and the
  source moved 8 horizontal units since the last probe, and every 1 s
  regardless. A pass casts at most 24 rays across all actors
  (`kMaximumGroundProbesPerPass`, reset by `ActorMovement::BeginPass` before
  the peers sync); an actor over the budget stays due and is served by a later
  pass, so an actor probed recently yields to the others.
- **Hold.** While the source's controller is off the ground (a jump or a
  fall), there is no probe and the target holds, so a jump arc keeps its shape
  over the local ground. While it swims the target is 0, since water levels match
  between load orders far more often than ground does.
- **Smoothing.** The offset eases toward its target with a 0.15 s time
  constant; the first probe sets it at once, so a spawned actor is not seen
  sliding. A checkpoint more than 256 units from the previous one (a
  teleport) clears the offset and its probe state.
- `Remote actor ground offset target changed` logs the first probe, a change
  between hit and miss, and a target 16 units or more from the last logged
  one, with the hit layer.

### Direction

Vanilla locomotion selects forward, backward, and strafe clips only through
`Direction` (`BSCyclicBlendTransitionGenerator.fBlendParameter` over
`*_Direction_Blend`); there is no backward movement event. Skyrim copies each
actor's own movement direction into `Direction` through `ActorDirectionChannel`
after main-loop writes, which for a remote actor is always forward, so the
channel reports the network direction instead. Like Skyrim does for the player,
a direction change of at least 1/8 turn is cross-blended with `CyclicFreeze`,
then `CyclicCrossBlend` one graph update later, at most once per 0.25 s so the
0.2 s blend is not restarted while the source's `Direction` ramps. Smaller
changes between checkpoints are interpolated with the pose instead, and larger
ones step at the played checkpoint, so the cross-blend covers them rather than
a sweep through the clips in between.

- `Direction` and the channel carry the network direction while standing too,
  and standing keeps the cross-blended direction. The source's graph keeps its
  last `Direction` when it stops (idle checkpoints repeat it), and the
  directional `BSCyclicBlendTransitionGenerator` stays active while the actor
  stands: in game it was active at every start except an actor's first move,
  after up to 6.6 s standing. A start in another direction is therefore a
  change like one while moving and is cross-blended the same way. Writing 0
  while standing and cross-blending every start froze the forward clip for
  each strafe's blend, matching the pose flicker seen in game, and starts
  without any cross-blend looked choppy on quick reversals.
- The first move after a spawn, a jump, or a graph invalidation only records
  its direction.

### Turning

Remote actors never turn in place; heading comes from the network. Their own AI
still asks to face a package or combat target, which that heading never
satisfies, so Skyrim sends `TurnLeft`, `TurnRight`, `turnStop`, and `NPC_Turn*`
and records the request in `iSyncTurnState`. The firewall blocks the events, but
every vanilla idle turn state machine (`MT_Idle_TurnBehavior`,
`1HM_Idle_Behavior`, `Bow_Idle_Behavior`, `MagicReadyIdleTurnBehavior`, and
others) binds its start state to `iSyncTurnState` (0 right, 1 standing, 2 left).
Each time one reactivated, after a weapon transition, `IdleForceDefaultState`,
or a stop, it entered a turn clip it leaves only on the blocked `turnStop`.
`iSyncTurnState` is therefore held at 1 before each graph update, with a network
`turnStop` whenever the AI changed it, and `ActorTurnDeltaChannel` reports 0.

### Standing re-entry

Every vanilla standing/moving machine, from the base locomotion of each stance
(`MT_Default_Behavior`, `1HM_Standing_Locomotion_Behavior`,
`Bow_Standing_Locomotion_Behavior`, `DefaultMagicReadyBehavior`,
`SneakMagicReadyBehavior`) to `Swim_Behavior`, `Torch_Ready_Behavior`, the block,
bow, and cast lower bodies, and the equip and unequip lower bodies, binds its
start state to `iSyncIdleLocomotion`, with standing as state 0 and moving as
state 1. No vanilla behavior node writes the variable, so on a remote actor it
would stay 0 and each of these machines entered while moving would start
standing. The
receiver writes `iSyncIdleLocomotion` every pass with the other variables: 1
while the dispatched state moves, 0 otherwise. A graph without the variable
only logs `syncIdleVariable=false`; it does not hold the dispatch.

- No one-shot `moveStart` follows a sneak edge taken while moving, a block,
  bow draw, or cast started while moving, or a weapon transition: with the
  hold, captures showed such repairs rejected or redundant every time (0 of
  46, 0 of 14, and 11 of 11 already moving).
- `Swim_Behavior` keeps its `moveStart` after `SwimStart`
  ([Swimming](#swimming)); it is skipped once the selectors read moving.
- The locomotion, sneak, jump, and landing machines (`MT_Locomotion_Behavior`,
  `Melee_Locomotion_Behavior`, `Sneak_Locomotion_Behavior`,
  `JumpLand_Behavior`, ...) start in the sprint or non-sprint state stored in
  `iSyncSprintState`, and only some write it back. A sprinting takeoff leaves
  it at sprint, so later walking, running, and backward movement would keep
  the sprint clip. The receiver writes `iSyncSprintState` from the replicated state with the
  other variables.
- When a landing completes, an attack, bow shot, cast, or block ends, or swimming
  stops (`locomotionReassertReason`: `landing`, `attack`, `bow-shot`, `cast`,
  `block`, `swim-stop`), the next dispatch asserts the target's locomotion edges
  once no attack plays and the actor no longer swims. Edges the graph already
  holds are skipped ([Stance reconciliation](#stance-reconciliation)), so only
  missing ones are sent. A pending reassert also counts as owning the graph for hand presentation
  starts, attack dispatch, and stance reconciliation. It logs `reasserted
  locomotion` with its reason.
- A standing re-entry none of these covers, such as a force equip's standing
  branch, is repaired by [stance reconciliation](#stance-reconciliation).

### Stance reconciliation

A sneak or move edge that ran out of retries is latched whether or not the
graph took it, edges are sent only when the latched state changes, and a branch
entered standing ([Standing re-entry](#standing-re-entry)) leaves the graph
standing under a moving latch. The graph would keep the wrong stance until the
source changed state again. A bow draw's equip branch rejecting `sneakStop` for
the whole retry window was logged leaving an actor crouched while its source
stood and walked, and a one-handed force equip that entered its standing branch
rejected `moveStop` for the whole retry window while already standing.

- `GraphStance::ReadStanceSelectors` reads the three axes in one walk of the active
  nodes. In the vanilla third-person behaviors, every `SneakStart`,
  `SneakStop`, `moveStart`, `moveStop`, `SprintStart`, and `SprintStop`
  transition belongs to a two-state machine that picks a default or a sneak
  variant (`MT_LocomotionType_Behavior`,
  `Melee_DirectionType_Behavior`, `Weap_IdleType_Behavior`,
  `BowLoweredType_Behavior`, `MagicReadyRootBehavior`, the equip and block
  selectors, ...) or a standing or moving variant (`MT_Default_Behavior`,
  `1HM_Standing_Locomotion_Behavior`, `Bow_Standing_Locomotion_Behavior`,
  `DefaultMagicReadyBehavior`, `SneakMagicReadyBehavior`, the cast, block, and
  bow lower bodies, `Swim_Behavior`, `Torch_Ready_Behavior`, and the equip and
  unequip lower bodies), or a locomotion or sprint variant
  (`MT_Locomotion_Behavior`, `Sneak_Locomotion_Behavior`,
  `Melee_Locomotion_Behavior`, `MagicReadyLocomotion_Behavior`, the cast, sneak
  magic, torch, and unarmed arm pose locomotion). The machine's current state switches when the event
  is taken, while the branch it leaves still blends out. Turn, shout,
  first-person, horse, flinch, stagger, bleedout, drunk, and crossbow aim and
  reload selectors are left out, and so are the magic locomotion speed
  samplers, which re-enter idle inside a moving branch, the jump and landing
  sprint variants, which linger while a landing blends out, and machines that
  pick a variant only on entry. Each axis reads off or on when every active selector
  of it agrees, mixed when they disagree (one stance's branch blending into
  another's), and none when no selector is active.
- `ReadGraphSneak` uses the sneak selectors and, with none active (a custom
  graph, or a branch without one), falls back to the variables: sneaking when
  `IsSneaking` is true or `iState` is 2, and not sneaking only when both are
  readable and neither holds. Those variables stay set while a sneak branch
  blends out after `SneakStop`: in a replay where the source toggled sneak
  every 0.15–0.2 s while running, the next `sneakStart` was skipped as already
  entered while the graph was leaving sneak, and the following `sneakStop` was
  then rejected for the whole 0.75 s retry window, with translation held and
  the actor running upright while latched sneaking. The vanilla sneak branches
  set `IsSneaking` through `BSIsActiveModifier` and tag `iState` 2 through
  `BSiStateTaggingGenerator`; the magic-ready and casting sneak branches only
  tag `iState`, the unarmed, weapon, and bow sneak idles only set `IsSneaking`,
  and transitional branches such as weapon equip set neither, so a
  not-sneaking reading counts only while nothing else owns the graph. Standing,
  moving, and sprinting have no variable fallback: `bInMoveState` and
  `iState` do not tell standing and moving apart in every stance
  ([Graph probe](#graph-probe)), so without a selector their edges are always
  sent.
- The dispatch reads the selectors once before sending any event of the pass,
  and skips an edge into the state the graph already holds: `moveStop` while
  standing, `moveStart` while moving, `SprintStop` while not sprinting,
  `SprintStart` while sprinting, `sneakStop` while the sneak selectors read
  not sneaking, and `sneakStart` while the graph reads sneaking by selectors or
  variables. Skyrim rejects such an edge, which held the dispatch for the whole
  locomotion retry window. The variables alone never skip `sneakStop`, since
  they also read not sneaking in branches without a sneak transition. A
  skipped `moveStart` does not count as entering `Sprint_State` for a rejected
  `SprintStart`.
- `ReconcileGraphStance` samples the graph every 0.1 s while the dispatch is
  latched on the target and nothing else owns the graph: grounded and not
  sprinting, no attack or locomotion reassert pending, no unsettled weapon
  transition, and no bow draw, cast, or block.
  - A sneak disagreement held for 0.5 s, longer than a `sneakStop` blend
    through the variable fallback, re-latches the opposite sneak variant of the
    target (`ToggleSneak`).
  - Standing selectors under a moving target for 0.2 s (two samples, since the
    selector switches as the edge is taken) re-latch the standing variant of
    the target (`ToStanding`). Moving selectors under an idle target are left
    alone: the graph rejected `moveStop` once `Speed` was zeroed, and `Speed` is
    written 0 every pass while the target is idle.
  - The next dispatch then sends the missing edge with the usual ordering,
    and retries. Each axis does this at most three
    times per target state, so a custom graph that reports stance differently
    cannot keep sending edges. A sneak correction takes the sample; the moving
    axis waits for the next.
- Dispatch logs carry the pre-event `graphMoving` (`standing`, `moving`,
  `mixed`, `unknown` with no selector active, or `unread` when no move or sneak
  edge was required), `moveAlreadyStarted`, `moveAlreadyStopped`, `sprintAlreadyStarted`,
  `sprintAlreadyStopped`, and
  `graphSneak` with its source (`sneaking/states`, `notSneaking/states`,
  `mixed/states`, `sneaking/variables`, `notSneaking/variables`, or
  `unknown`), `sneakAlreadyEntered`, and `sneakAlreadyLeft`. A correction logs
  `graph sneak disagrees with the latched state` or `graph stands while the
  latched state moves`. A mixed reading logs `stance selectors disagree` with
  every active selector and its state, at most on the first and every 60th
  retry of an edge.

### Beast graphs

A remote actor in a [beast form](appearance-and-equipment.md#shapeshifts)
plays `WerewolfBehavior.hkx` (also the werebear's) or `VampireLord.hkx`, whose
locomotion inputs match the human ones: `moveStart` and `moveStop`,
`sprintStart` and `sprintStop`, the jump and landing events, `SwimStart` and
`SwimStop`, and `weapEquip` and `weapUnequip`, so the same dispatch drives
them. The string pool folds case, so `SprintStart` names `sprintStart`.

- Neither graph has a `SneakStart` transition: `SetAnimationState` turns a
  beast's sneaking target into its walking, running, or idle variant, so no
  sneak edge is sent and none is reconciled.
- The selectors of `GraphStance` also list the beast graphs: standing or
  moving in `MovementBehavior` (both), `Behavior03` and the swim machine
  `Behavior06` (werewolf), and `CombatReadyBehavior`, `EquipBehavior`,
  `UnequipBehavior`, `LevitationMovementBehavior`, `LevitateCombatBehavior`,
  and `CastState_Ready_Behavior` (vampire lord); sprinting in `H2HBehavior`
  (werewolf), `Ground_StateBehavior`, and `Levitation_Behavior` (vampire
  lord). A fourth axis reads the vampire lord's levitation from its
  `H2HBehavior`: `Ground_State_Cam` and `Levitate_End` off, `LevitationBase_State`
  and `Levitate_Start` on, so a toggle counts as soon as it is taken
  ([Combat presentation](combat-presentation.md#levitation)). A machine name
  can carry several selectors; a state of another graph's machine of the same
  name matches none of them.
- A beast's draw and sheathe have no clip exit annotation (`weapEquip` takes
  the werewolf straight into `CombatIdle`), so the transition completes once
  Skyrim's weapon state reaches drawn or sheathed, or at the usual 2.5 s
  bound; no exit event is forced and `IdleForceDefaultState`, which the beast
  graphs lack, is never sent.
- The idle turn machines number their states as the human ones do (1
  standing), so the [idle turn hold](#turning) applies unchanged.

### Graph probe

A diagnostic for finding a graph reading that tells a standing actor from a
moving one in every stance; `iState` does not (spell-ready locomotion reads 9
standing and moving). Nothing acts on the probe lines; the stance selector read
shares their layout reads ([Stance reconciliation](#stance-reconciliation)).
The probe is compiled only when `NDEBUG` is not defined (the XMake `debug`
mode, not `releasedbg`): each probe builds and logs several kilobytes under the
graph lock and keeps a copy of the graph's variables per actor, which does not
scale to hundreds of actors.

A replay (unarmed, one-handed, dual wield, bow, magic, sneak, sprint, jumps)
showed that no variable does: `bInMoveState` is set only in the weapon
and bow locomotion and sprint states and reads 0 in running magic, unarmed,
and sneak locomotion, and `iState` reads 0 for both unarmed states and 9 for
both magic states. The active state names do, one machine below the stance
root: `MT_Default_Behavior` `MT_Standing_State` or `MT_LocomotionType_State`,
`1HM_Standing_Locomotion_Behavior` and `Bow_Standing_Locomotion_Behavior`
`*_Standing_State` or `*_Locomotion_State`, and `DefaultMagicReadyBehavior`
`MagicReadyStandingState` or `MagicReadyLocomotionState`.

- `Animation graph probe` lines carry the latched state, `iState`, and from
  `GraphProbe::Describe`: every active `hkbStateMachine` with its current
  state's name, every active `hkbClipGenerator`'s animation, and the root
  graph's word-sized variables (bool, int, real) that changed since the
  actor's previous probe, or all of them on the first probe of a graph.
- `reason=state` is logged once the latched state changed and the dispatch
  stayed settled for 0.5 s with no attack, hand presentation, unsettled weapon
  transition, or recovery; `reason=reassert` and
  `reason=weapon-replay` the same way after a locomotion reassert or the
  replay that ends a weapon transition. A
  `reason=unsettled-translation` probe follows the `resumed translation while
  its locomotion dispatch is unsettled` line at once.
- It reads `hkbBehaviorGraph::activeNodes` and `variableValueSet` under the
  graph manager's `updateLock`, with two Havok 2010 layouts CommonLib leaves
  unnamed: the value set's word array right after `hkReferencedObject`, and a
  state info's name at 0x60 and state ID at 0x68. The stance selector read
  walks the same active nodes and recognizes a state machine by an exact
  `VTABLE_hkbStateMachine` match instead of an RTTI cast per node, since
  reconciliation reads it every 0.1 s per actor.

### Jumps

- Takeoff restores `bInJumpState` and the jump sample's variables, then sends
  `JumpStandingStart` or `JumpDirectionalStart`.
- Landing waits for the airborne actor to travel on to the landing checkpoint's
  position (`ActorMovement::State::checkpointPosition`, plus the
  [ground offset](#ground-offset)), which the pose
  reaches as that checkpoint plays, bounded to 0.25 s (then it lands on the
  pose), aligns the actor there, restores `bInJumpState=false` and the current
  locomotion variables, and only then sends `JumpLand`.
- The applied jump value is reasserted immediately before and after the normal
  actor update, so the locally grounded controller cannot overwrite it.
- A graph output-event sink watches for the delayed `JumpLandEnd` annotation.
  The landing edge is complete only once it is seen, or after a 2 s fallback
  that sends `JumpLandEnd` itself for a custom graph without the annotation;
  every active grounded axis is then reasserted even if the movement state did
  not change, so the actor cannot leave the branch in a sliding idle pose. The
  dispatch history is kept: forgetting it would read a drawn weapon as a new
  draw and arm a weapon-transition replay that fires mid-jump. Translation continues during the wait, while
  locomotion edges are held because the jump branch rejects them.
- A jump is not started while the actor is swimming: interpolation can put the
  actor at swim depth while its source still wades, and a jump there would play
  in `Swim_State`.

### Jump sounds

The vanilla jump states emit the `JumpUp` and `JumpDown` graph events, which
Skyrim plays as footstep sounds of the actor's footstep set
(`DefaultFootstepJumpUpImpact` 0x5A288, `DefaultFootstepJumpDownImpact`
0x12F0E), picked by ground material. A non-player actor gets each impact's
first sound at its position through a shared handle pool. Every takeoff sound
and the stone-like landing sounds use `SOMMono01400Player1st` (0xB4058), a
fixed-speaker output model that does not attenuate, so without a fix every
remote jump is heard at full volume anywhere. Vanilla NPCs rarely jump.

`JumpSound` fixes this for remote actors only; the local player's sounds and
the shared records are untouched:

- The footstep sink (ID 36209) marks a registered actor's `JumpUp` or
  `JumpDown` event for the thread dispatching it. Other footsteps only pay a
  pooled-string comparison.
- Its call to `PlayImpactDataSounds` (ID 36209 + 0x53D) leaves the sound alone
  unless the selected sound's `BGSStandardSoundDef` output model lacks
  `kAttenuatesWithDistance`. Otherwise it scales the volume by the distance
  from the audio listener (`BSAudio::audioListener`) to the sound position,
  with the curve of `SOMMono01800`, the model of vanilla NPC landings on soft
  ground: full volume to 150 units, then 100, 50, 20, 5, and 0 % evenly to
  1800 units. Beyond that the sound is not played.
- The pool's call to `BSSoundHandle::Play` (ID 36208 + 0x182) sets that volume
  on the pooled handle just before play.

The volume is fixed when the sound starts. The play hook is installed first and
the others only if it succeeded, so a nearby jump is never routed without its
volume.

### Swimming

Skyrim manages the swim transitions locally; the firewall admits `SwimStart` and
`swimStop` ([Remote actor](remote-actor.md#swimming) keeps the controller
support consistent with the source).

- `Swim_Behavior` enters `SwimIdle` or `SwimLocomotion` from
  `iSyncIdleLocomotion` ([Standing re-entry](#standing-re-entry)) and leaves
  `SwimIdle` only on `moveStart`. When
  `SwimStart` is accepted while both dispatched and target locomotion move, the
  pass waits two main-loop passes for the swim branch, then sends `moveStart`,
  unless the selectors already read moving, until it is accepted, the actor stops or leaves the water, or 0.5 s pass. A
  swim started standing needs nothing: the later move sends its own `moveStart`.
- After `swimStop` is accepted and the actor leaves the swimming state, the
  current locomotion events are reasserted, so a moving actor walks instead of
  sliding in an idle pose.
- `SwimStart`, `JumpLand`, and `JumpLandEnd` are wildcard transitions of the
  root state machine, and `JumpLandEnd` leads to land locomotion from
  `Swim_State` too. A jump into water reaches the network as a landing at the
  surface, and Skyrim sends `SwimStart` just after (observed in game), so the
  landing clip never emits `JumpLandEnd`. While the actor swims, a pending
  landing ends without its fallback or locomotion reassert, and a landing that
  arrives after `SwimStart` sends no `JumpLand`. Otherwise the actor walks on
  the riverbed.

## Engine assumptions

Read from the 1.7.104 executable, `Skyrim.esm`, and the vanilla behavior files,
not confirmed in game:

1. The footstep sink plays a non-player actor's impact sound through
   `ImpactSoundData::pool` and its first sound only; the player takes the
   second sound and no pool.
2. `BSSoundHandle::SetVolume` before `Play` scales a sound with a
   non-attenuating output model, and its message reaches the audio thread
   before the play message.
3. An output model's five curve values are evenly spaced from its minimum to
   its maximum distance.
4. A `BSIsActiveModifier` output returns to its inverted value when its branch
   deactivates (vtable `deactivate` writes it), as the engine-read outputs
   `bAnimationDriven`, `IsStaggering`, and `IsRecoiling` require, so a true
   `IsSneaking` means a sneak branch is active.
5. A stance selector's `currentStateID` changes as soon as the graph takes its
   `SneakStart`, `SneakStop`, `moveStart`, or `moveStop` transition, before the
   blend completes, and a sneak selector entered later starts in the variant
   matching the actor's stance.
6. Every hkbStateMachine node in `activeNodes` has exactly the
   `hkbStateMachine` vtable (no engine subclass).
7. A state machine reads its bound `startStateId` when it is entered, so a value
   of `iSyncIdleLocomotion` written in the main loop applies to every later
   entry, and nothing in the engine writes the variable for an actor that is not
   paired with another (the only engine reference by name is the
   `ActorCopyGraphVariableChannel<int>` built in ID 40358, a function that takes
   two actors).
8. The beast graphs take the human locomotion inputs as the human graph does
   (`moveStart` entering `Locomotion_State`, `sprintStart` the sprint state,
   the jump events the jump branch), and Skyrim's weapon state of a beast
   reaches drawn and sheathed after a native draw and sheathe.
9. A player who keeps a direction key held through a loading screen keeps
   its movement bits set and moves once control returns, which the
   [arrival hold](#source-capture) releases by distance; not recorded.

Ground offset, read from the 1.7.104 executable and `Skyrim.esm`; the pick
path and the player's `kOnGround` behaved as below in game on 1.7.104, and the
horse's controller is unconfirmed:

10. `bhkWorld::PickObject` (vfunc 0x33, ID 77860) takes the world's read lock
    (ID 68233 on `bhkWorld + 0xC598`) when Havok runs multithreaded, casts with
    the collector in `bhkPickData::closestRayHitCollector` through
    `hkpWorld::castRay` (ID 61400) after resetting its hit, and copies the
    collector's hit to the pick's output when `rootCollidable` is set. A
    non-zero `bhkPickData::ray` replaces `to` with `from + ray`.
11. The ray's layer, `L_CUSTOMPICK1` (43), collides with every layer
    `GroundProbe` accepts (static, animated static, transparent, clutter,
    trees, props, terrain, ground, and the two small transparent layers), per
    the COLL records; `L_LOS` misses transparent and `L_PATHINGPICK` clutter
    and props. Water, triggers, invisible walls, and characters are hit but
    skipped by the collector, which leaves its early-out fraction alone so the
    ray continues past them. A remote actor's own body has the no-collision
    bit ([Remote actor](remote-actor.md#character-body)).
12. A hit's fraction is along the whole world ray, and the player's proxy
    controller and a horse's rigid-body controller report `kOnGround` while
    standing or moving on the ground, briefly leaving it on steps and slopes
    (the receiver holds the offset then).

The graph probe's value-set and state-info offsets come from the Havok 2010
class layouts and were not checked against the executable; in game they read
correct names and values, and `activeNodes` listed the active nodes of the
referenced sub-behaviors when read from the main loop.

## Acceptance criteria

1. A remote actor follows its source's path, walking, running, sprinting, and
   sneaking with the source's clips and speed, without sliding in an idle pose.
2. Backpedalling and strafing play directional clips while the actor keeps
   facing the source's heading. The actor never plays a turn-in-place clip.
3. Standing, directional, and repeated jumps take off and land where the source
   did, returning to the right gait, including after a sprinting jump.
4. Swimming, leaving the water, and jumping into water play swim and land
   locomotion without walking on the riverbed.
5. Dropped intermediate checkpoints converge to the newest state.
6. With a 20 Hz server tick, an actor walking, running, sprinting, or sneaking
   moves at an even pace along its source's path, without periodic stops or
   surges, including at 30 and 144 fps.
7. A remote jump is heard near the actor, fades with distance, and is silent
   beyond about 1800 units on stone, dirt, grass, snow, wood, and water. The
   local player's own jump sounds are unchanged.
8. A remote actor that draws a bow, weapon, or spell while sneaking and then
   stands up, walks, or runs is not left crouching, and one that sneaks during
   a draw does not stay standing.
9. A remote werewolf, werebear, or vampire lord walks, runs, sprints (on all
   fours as a werewolf), jumps, and swims with its own clips without sliding,
   draws and sheathes its claws, and a vampire lord does so hovering and on
   the ground.
10. A player who runs through a door or fast travels appears standing in the
    new space until it moves there.
11. Where the peers' ground differs by up to about 96 units up or 128 down
    (landscape mods), a remote actor or mount walks, stands, and lands on the
    local ground without floating or sinking, and its jumps keep their arc.

## Known limitations

- Look and aim direction independent of the body is not replicated.
- Jitter beyond the playout delay leaves nothing to interpolate toward: the
  actor stops at the played checkpoint and catches up once the next one plays
  ([Sync pipeline](sync-pipeline.md#checkpoint-playout)).
- A mod or behavior-graph mismatch between peers is not detected; missing
  variables and rejected or timed-out events are logged.
- The stance selectors are recognized by their vanilla machine and state
  names. A graph that renames them falls back to the sneak variables, with
  their lag after `SneakStop`; reconciliation then needs a graph that sets
  `IsSneaking` or tags `iState` 2 in its sneak branches, and with neither a
  timed-out sneak edge stays latched. Such a graph gets no move skips or
  standing correction.
- A graph left moving under an idle target is not corrected.
- An attenuated jump sound keeps its fixed-speaker output: it fades with
  distance but has no direction.
- The [ground offset](#ground-offset) leaves a source standing on something
  missing locally at its own height, floating, rather than putting it on the
  ground below; that ground can be a riverbed under a bridge or the inside of
  a building. A larger difference than the probe window is not corrected
  either. Clutter counts as ground where it lies in the local world, so a
  source standing on a crate that was moved in one world stands on the ground
  there, or on the crate where the source has none.
- The offset is sampled at most every 0.1 s and eased, so on ground whose
  difference changes quickly the actor's feet lag it slightly. Landing on
  ground of another height eases over the 0.15 s smoothing after the touch.
- Recordings made before the `grounded` flag replay without the offset.
