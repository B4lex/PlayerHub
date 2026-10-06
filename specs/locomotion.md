# Locomotion

## Status

Implemented and run in game on Skyrim AE 1.7.104 with live and replayed
players: position, heading, directional movement, walk, run, sprint, sneak,
jumps, and swimming. The behavior-graph facts below were read from the vanilla
`Skyrim - Animations.bsa` behavior files or observed in game, as noted.

Following the interpolated pose of the [checkpoint
playout](sync-pipeline.md#checkpoint-playout) was run in game with a 20 Hz
server tick: walking and running actors moved at an even pace. Following idle
drift instead of snapping was in the same build and looked better, but was
not checked separately, nor were slow movement at a high frame rate and the
slide after a stop. The idle drift figures below were measured from the 13
server recordings.

Jump sound attenuation: hook sites traced in the 1.7.104 executable and the
sound records read from `Skyrim.esm`; Windows build and in-game behavior not
yet verified. Other runtimes skip it with a warning.

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
its previous local state. Vanilla humanoid graphs are the baseline; custom
graphs may slide or miss transitions.

## Components

| Module | Responsibility |
| --- | --- |
| `sync/movement/MovementSync` | Source transform sample; receiver transform target and coordinate-space checks |
| `sync/animation/AnimationSync` | Source movement state, jump, stance, and graph epoch; receiver staging and the translation gate |
| `actor/ActorMovement` | Direct translation and heading on the actor |
| `actor/ActorAnimation` | The actor's graph state machine: variables, events, staging, retries, landing, swim, weapon and presentation transitions, recovery |
| `actor/JumpSound` | Distance attenuation of the actor's takeoff and landing footstep sounds |

`ActorAnimation` is one controller on purpose: landing, draw and sheathe, swim,
recovery, and attack transitions share one graph lifecycle.

## Source capture

At each checkpoint, from the carrier (the player, or its mount while riding):

- **Position** and **heading**. Heading is the body angle, so a backpedalling or
  strafing player keeps facing the same way. Skyrim heading zero faces +Y.
- **Speed** is measured from the previous published sample. A jump of more than
  4096 units is a relocation, not a speed sample.
- **Direction** is travel relative to heading in graph `Direction` units
  `[0, 1]` (0 forward, 0.25 right, 0.5 backward, 0.75 left), read from the
  source graph's `Direction`, or derived from horizontal travel when a custom
  graph lacks it.
- **Movement state**: `idle`, `walking`, `running`, `sprinting`, and the three
  sneaking states, from the player's `ActorState`.
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
- **Graph epoch**: observed every pass, and advanced when the player's graph
  manager is replaced or becomes unavailable
  ([Combat presentation](combat-presentation.md#action-stream)).

## Receiver

### Coalescing

Locomotion and jump phase are current state, not action history. Checkpoints
play at their sampled spacing ([Sync pipeline](sync-pipeline.md#checkpoint-playout)),
and speed, direction, movement state, and jump phase are applied from the
played checkpoint; when several play in one pass, only the last is applied.
Position, heading, and pitch follow the pose interpolated from the played
checkpoint toward the next, so they never run ahead of the movement state
sampled with them. Replaying skipped states would make the graph traverse
obsolete states, and pairing a newer transform with an older locomotion state
was an early cause of sliding. Takeoff and landing events are derived only
when the applied checkpoint changes jump phase; no jump events are queued.

### Graph state

- Variables `Speed`, `SpeedSampled`, `Direction`, `TurnDelta`, `IsSprinting`, and
  `iSyncSprintState` are refreshed every pass with the checkpoint's speed and
  direction, in the same pass as translation and before it. `Speed` uses the
  checkpoint speed, with the actor's own speed only as a fallback for an invalid
  or zero sample while moving.
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
  attack's exit was measured holding it past 0.1 s.
- A newly created or reloaded graph receives the current state.
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
- Position approaches the pose (`ActorMovement::SetTargetTransform`, every
  pass) at the pose's path speed plus a backlog drain of the remaining distance
  over about 0.5 s, so an actor on the path follows it exactly and one held
  behind it, by a translation hold or a pause in arrivals, catches up without
  a snap. `deltaTime` is clamped and a step never overshoots the pose. While
  moving there is no arrival tolerance: at a slow walk and a high frame rate
  the pose advances less than one unit per pass, and a tolerance made the
  actor move only every other pass.
- Idle players still move: in the recordings, 31% of idle checkpoints moved
  0.5–50 units, mostly a horizontal drift of about 20 units per second, and the
  first idle checkpoint after a stop carries the deceleration (median 2 units,
  up to 22). Snapping to the target once it was more than 2 units away (the
  1-unit stopping distance and tolerance) moved such an actor in hops of 2–3
  units every ~0.1 s. Once the idle graph state settles, an idle actor instead
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
  it still carries forward; without this the actor fell behind and teleported
  on the idle checkpoint.
- `SetPosition(next, true)` moves the reference and its controller. Heading
  turns toward the pose's heading at up to one turn per second.

### Direction

Vanilla locomotion selects forward, backward, and strafe clips only through
`Direction` (`BSCyclicBlendTransitionGenerator.fBlendParameter` over
`*_Direction_Blend`); there is no backward movement event. Skyrim copies each
actor's own movement direction into `Direction` through `ActorDirectionChannel`
after main-loop writes, which for a remote actor is always forward, so the
channel reports the network direction instead. Like Skyrim does for the player,
a direction change of at least 1/8 turn is cross-blended with `CyclicFreeze`,
then `CyclicCrossBlend` one graph update later, at most once per 0.25 s so the
0.2 s blend is not restarted while the source's `Direction` ramps.

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

Several state machines re-enter at a standing state chosen from
`iSyncIdleLocomotion`, which stays 0 on a remote actor:

- Vanilla `JumpLandEnd` exits into that standing state, so a landing has no
  `SprintStop` edge until `moveStart` re-enters locomotion.
- The locomotion, sneak, jump, and landing machines (`MT_Locomotion_Behavior`,
  `Melee_Locomotion_Behavior`, `Sneak_Locomotion_Behavior`,
  `JumpLand_Behavior`, ...) start in the sprint or non-sprint state stored in
  `iSyncSprintState`, and only some write it back. A sprinting takeoff left it
  at sprint, so later walking, running, and backward movement kept the sprint
  clip. The receiver writes `iSyncSprintState` from the replicated state with the
  other variables.
- With a spell readied, locomotion runs in `MagicReadyRootBehavior`, whose
  `DefaultMagicReady` and `SneakMagicReady` branches re-enter their standing
  machine. A `SneakStart` or `SneakStop` taken while moving left the actor
  standing while it translated, and the next `moveStop` had no edge. The
  receiver sends one `moveStart` on the graph update after a sneak edge taken
  while moving; the unarmed and melee graphs keep sneak inside locomotion and
  reject it.
- Hand presentation lower bodies behave the same way
  ([Combat presentation](combat-presentation.md#moving-lower-body)).

### Jumps

- Takeoff restores `bInJumpState` and the jump sample's variables, then sends
  `JumpStandingStart` or `JumpDirectionalStart`.
- Landing waits for the airborne actor to travel on to the landing checkpoint's
  position (`ActorMovement::State::checkpointPosition`), which the pose
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
  `iSyncIdleLocomotion` and leaves `SwimIdle` only on `moveStart`. When
  `SwimStart` is accepted while both dispatched and target locomotion move, the
  pass waits two main-loop passes for the swim branch, then sends `moveStart`
  until it is accepted, the actor stops or leaves the water, or 0.5 s pass. A
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
  arrives after `SwimStart` sends no `JumpLand`. Otherwise the actor walked on
  the riverbed.

## Engine assumptions

Read from the 1.7.104 executable and `Skyrim.esm`, not yet checked in game:

1. The footstep sink plays a non-player actor's impact sound through
   `ImpactSoundData::pool` and its first sound only; the player takes the
   second sound and no pool.
2. `BSSoundHandle::SetVolume` before `Play` scales a sound with a
   non-attenuating output model, and its message reaches the audio thread
   before the play message.
3. An output model's five curve values are evenly spaced from its minimum to
   its maximum distance.

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

## Known limitations

- Look and aim direction independent of the body is not replicated.
- Jitter beyond the playout delay leaves nothing to interpolate toward: the
  actor stops at the played checkpoint and catches up once the next one plays
  ([Sync pipeline](sync-pipeline.md#checkpoint-playout)).
- A mod or behavior-graph mismatch between peers is not detected; missing
  variables and rejected or timed-out events are logged.
- An attenuated jump sound keeps its fixed-speaker output: it fades with
  distance but has no direction.
