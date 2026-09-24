# Mount Riding

## Status

Implemented. The server side is covered by unit tests. In game, a replayed
rider was seated on its horse, and the horse walked, trotted, and ran with the
recorded speed. Dismounting and death removed the horse without a crash. The
gallop is a known limitation; see [Known limitations](#known-limitations).
Horse jumps are out of scope.

The mount and dismount animations ([Transitions](#transitions)) are implemented
and covered by server tests. In game, replayed riders played both animations on
their horse's spot, repeatedly and without a crash, and walked on after each
dismount without a respawn.

## Goal

When a player rides a mount, peers see the remote actor sitting on a horse that
follows the player's path at a matching gait. The mount exists only while the
player rides and is removed with its rider in every case: dismount, death,
leaving the world, disconnect, timeout, and any retirement of the actor.

## Non-goals

- The mount's appearance. Every remote mount is the vanilla
  `EncHorseSaddledBrown` (`Skyrim.esm:023AB2`), whatever the player rides, so it
  exists in every modpack.
- Combat on horseback: drawn weapons, attacks, bows, and spells. Attacks queued
  for a riding player are dropped, and hand presentation is not applied.
- Horse jumps, and a mount left standing after a dismount: the horse is removed
  once the dismount animation ends and the player no longer rides.
- The mount's approach. A peer's horse appears when its player starts to mount.

## Protocol

`player:update` and `player:state` carry `mounted`, a boolean that is true while
the player rides. While it is true, `x`, `y`, `z`, `heading`, `speed`, and
`direction` describe the mount rather than the rider. The receiver drives the
mount from them, and Skyrim carries the rider on the mount. The server validates
`mounted` as a boolean and defaults it to `false` when missing, so older
recordings replay on foot. Every relayed `player:state` includes it, and the
client requires it.

Getting on and off with an animation are separate events, since a checkpoint
field would only say that the player rides, not how it started. The client emits
`mount:transition` with `transition` (`enter` or `exit`), `graphEpoch`, and
`checkpointSequence`, right after the `player:update` it is anchored to. The
server accepts it only against the player's latest checkpoint: the same graph
epoch, an anchor no newer than that checkpoint, which must be mounted and alive,
and an anchor newer than the previous transition's. Both transitions start while
the checkpoint rides: `mounted` turns true when the mount animation starts, and
false once the dismount animation has ended. It relays the transition as
`player:mount` with `playerId`, and records it. A replay anchors it to its own
latest checkpoint, and a seek never replays one it skips.

## Source capture

`MountSync::SampleLocalMount` reads the local player's mount once per published
checkpoint (`IsOnMount` and `GetMount`). `MovementSync::SampleLocalMovement`
then samples position, heading, speed, and graph `Direction` from that mount
instead of the player. Speed is still measured against the previous published
sample, so mounting and dismounting cost one speed sample across the offset
between rider and mount. The player is reported mounted for the whole
interaction, including its mount and dismount animations.

The same sample observes the transitions from the player's sit state while it
is on a mount. The rider graph's `HorseEnter` sets `kWaitingForSitAnim`,
`HorseExit` sets `kWantToStand`, and both return to `kRidingMount` or `kNormal`
once their clip ends (ID 39004). A mount whose first samples are `kWantToSit` or
`kWaitingForSitAnim` publishes `enter`. A later `kWantToStand` publishes `exit`.
An instant seat, such as a loaded save, goes straight to riding and publishes
nothing.

## Receiver

`MountSync::SyncMount` runs in the remote pass after the death and stranded
actor checks, and before every stage that drives the actor on foot:

1. While the newest checkpoint rides, and the player is not dead, a mount is
   spawned at the checkpoint through `ActorSpawner`, so it is registered like a
   remote actor. It has survival, combat, magic, hostility, and pass-through
   collision isolation, and its activation is blocked, so it cannot be ridden
   or stolen locally.
2. The mount takes each new checkpoint as its target transform. Once ready,
   `ActorRuntime::UpdateMount` reasserts its policy, drives its gait, and
   translates it with the actor translation code. Its graph is never firewalled.
3. Once both actor and mount are ready, `ActorMount::Seat` seats the actor,
   retrying every 0.5 s. It plays the mount animation for a received `enter`
   (see [Transitions](#transitions)), and is instant otherwise. From the first
   seat on, the mount carries the actor: it is never driven on foot again.
   `ActorRuntime::UpdateRider` only keeps its policy, equipment and appearance
   are still reconciled, and queued attacks are dropped.
4. The seat is checked every 0.5 s, since the check looks up the rider's
   interaction. A rider found off its mount is logged and seated again.
5. When the checkpoint no longer rides without an animated dismount, the actor
   and its mount are retired together, and the actor respawns on foot. The
   actor is not taken off the mount first (see below). A new seat also waits
   while either actor is getting on or off a mount.
6. A mount whose handle is lost retires its actor. Both are rebuilt while the
   player still rides.

`ActorLifecycle` owns the mount beside the actor. Every retirement and removal
of the actor also removes the mount in the same pass, rider first, while the
rider is still seated. A pair that a mount or dismount clip links is removed
once the clip ends instead (see [Transitions](#transitions)). That covers appearance, coordinate space, fast travel,
revival, a lost actor handle, `player:exit`, `player:disconnect`, and a lost
connection. Mounts are excluded from saves like their riders. When a riding
player dies, `DeathSync` retires the actor and its mount the same way. The actor
respawns on foot, and is then killed where it stands like any actor spawned for
a dead player.

Skyrim's dismount (ID 38705) is only used for an animated dismount. Even in
mode 1, which sets the quick-mount process flag, it starts the paired
`pa_HorseExit` clip, a `BSSynchronizedClipGenerator` that links the mount's
graph to the rider's. Removing both actors in the pass that started it crashed
on the next graph event: execution jumped into freed memory from
`NotifyAnimationGraph`, with the mount and its `pa_HorseExit` clip on the stack.

## Transitions

A received transition waits in the peer's `MountSync::RemoteState`. One that
cannot start within 1 s of arriving, for example because the actor or its mount
is still loading, is dropped, and the transition is presented instantly as
above. A peer that joins, or enters the space, mid-ride never sees one.

Enter:

1. While an `enter` waits and the checkpoint rides, the actor is held where the
   player started mounting, rather than following the mount's transform on
   foot, and the mount is spawned. The enter is sent right after the checkpoint
   that starts riding, but may arrive in a later pass: in the first run the
   mount was ready 6 ms after that checkpoint, and the enter arrived 43 ms after
   it, so the rider was seated instantly. A new mount therefore waits 0.25 s for
   the enter before an instant seat, and the actor is held meanwhile.
2. Until the rider is first seated, the mount is sent no gait events, but is
   kept on its target transform with an idle translation, which places it
   there at once. In the first run, the relocation sample (1147 units/s)
   started the new mount's locomotion in the pass the mount animation started.
   In the next, the mount was held in place instead, and was sometimes pushed
   off its spot by the rider standing beside it: the mount animation then
   played there, and the mount slid back to its target once it ended. Kept on
   its target, the mount stood within 2 units of it whenever the animation
   started.
3. The animated seat waits until the mount has been ready for 0.25 s. The first
   run started it in the pass the mount was created, while its package was
   still being taken back, and the rider was off the mount within 0.5 s.
4. `ActorMount::Seat` then starts the same interaction as the instant seat, but
   not quick: Skyrim performs the mount action on the rider's
   graph, which plays the paired `HorseEnter` clip and picks the side itself.
   A graph that rejects it ends the interaction at once, so the next seat check
   finds the rider off its mount and seats it instantly.
5. While the clip plays, and for 0.1 s after it ends, the mount is neither
   translated nor sent gait events, and the rider's riding gait is not driven
   during the clip. In the pass the enter clip ended, the mount's graph rejected
   `moveStart`, so a player riding off at once had its mount slide until the
   gait was corrected 0.25 s later. `Remote player … mount clip
   started` and `ended` log each clip, and a lost seat logs whether it was
   animated, how long it lasted, and the rider's sit state.

Exit:

1. An `exit` starts once the rider is seated, both actors are ready, and no
   clip plays. `ActorMount::Dismount` calls the dismount in mode 0, the AI
   dismount procedure's, which plays `HorseExit` and ends the interaction when
   the clip does. It is started even when the checkpoint that no longer rides
   arrived first.
2. The mount is held while the clip plays. The dismount ends once no clip plays
   and the rider is off its mount. After 5 s it is treated as failed, and the
   actor and mount are retired.
3. The actor then stands beside the mount until the checkpoint no longer rides.
   In the next pass that sees it, the mount is removed alone. The actor is
   released to on-foot control: its registration is no longer mounted or
   gait-driven, and its locomotion dispatch is rebuilt from the checkpoint. A
   player that still rides 2 s after the dismount ended is seated again at once.

A clip is playing while either actor has `kGettingOnOffMount`, or while the
rider is on a mount with a `kWantToSit`, `kWaitingForSitAnim`, or `kWantToStand`
sit state. Any retirement or removal of an actor in that state, whether for
death, a coordinate-space change, `player:exit`, a disconnect, or a dismount
without the animation, hands the actor and its mount to a deferred removal.
They stay registered and undriven, and are removed once the clip ends. After
5 s they are removed anyway, which logs a warning. The player's new actor is not
spawned until then, so it never appears twice. Deferred actors are excluded from
saves like live ones.

The mount's gait comes from the checkpoint speed. Vanilla horse movement types
are walk 125, run 450 (`Horse_Default_MT`), and sprint 600
(`Horse_Sprint_MT`). Below 20 the mount is idle (`moveStop`, which also ends the
gallop). It starts galloping at 560 and stops below 490 (`SprintStart`,
`SprintStop`, `IsSprinting`), so a measured speed near either threshold does not
toggle it. `Speed` and `SpeedSampled` take the checkpoint speed, and `Direction`
stays zero. The graph variables are written once per new checkpoint.

Skyrim polls the graph's `Speed` through `ActorSpeedChannel`, from the actor's
own movement. That is zero for a mount moved by its transform. It caused the
first run's slide: `Speed` fell back to zero after each write, so the gait
played standing still. Like the `Direction` channel of every remote actor, the
`Speed` channel of a remote mount reports the network speed. The registry
counts mounts, so while no one rides, the channel hook of every local actor
returns without taking the registry lock. The second run confirmed that the
graph's `Speed` follows the network speed.

How `HorseBehavior.hkx` (vanilla) picks a gait, read from its packfile:

- `moveStart` enters `LocomotionState`, and `moveStop` leaves it.
- `ForwardLocomotionBlend_Horse` blends its clips on `HorseSpeedSampled`, in
  world units: slow walk 5, walk 125, slow trot 214, trot 304, run 450.
- The sprint behavior is active while `IsSprinting` is set; `SprintStart` and
  `SprintStop` switch to and from it.
- A `BSSpeedSamplerModifier` writes `HorseSpeedSampled` from `Speed`, `iState`,
  and `Direction`, through a global speed sampler Skyrim installs at startup.
  Without one, it copies `Speed`.
- The graph has no `SpeedSampled`, and `fMinSpeed` is 5.

In the second run the mount trotted and ran, but its walk had the wrong
cadence, and a `SprintStart` at 568 was rejected. The third run ruled out the
mount's graph: at walking pace it reported `IsMoving`, with `Speed` and
`HorseSpeedSampled` near 125, the WalkForward clip. No native gait event
reached it.

The fourth run found the cause: the mount galloped at every speed. The first
checkpoint after a mount spawns carries the relocation onto it (speeds of 1176
and 2442), and that sample started a gallop. `moveStop` then left locomotion
without leaving the gallop state inside it, and every later `moveStart` resumed
the gallop (`SprintForward`). So the blend never ran, although its variables
were right, and `SprintStart` was "rejected" because the graph was already
galloping. The gait is now reconciled against the graph's own state:

- The gallop starts only after the speed stays in its range for 0.3 s, so a
  single relocation sample cannot start it.
- `SprintStop` is sent before `moveStop`.
- An edge event records the request whether or not the graph accepts it.
- Every 0.25 s, the graph's `IsMoving` and its gallop are compared with the
  request. The gallop state tags `iState` with `iState_HorseSprint`. A mismatch
  sends `SprintStop`, `moveStart`, `moveStop`, or `SprintStart`, in that
  priority (`Corrected remote … graph gait`).

The rider has its own `horsebehavior.hkx` (under `actors\character`), with the
same locomotion blend, driven by its own `Speed`, `Direction`, and
`HorseSpeedSampled`. Once seated, the rider was no longer driven, so its graph
kept the `Speed` of its last on-foot update. Now its `Speed` and `IsSprinting`
follow the mount's speed, and its `Speed` channel reports it. Its `Direction`
channel reports zero, since its last on-foot direction is stale, and seating
clears its authoritative jump state. The rider's graph rejects `moveStart` and
`moveStop` (over 2000 times in the fourth run), so it is sent no locomotion
events. The rider takes every native event, and those reach it from its mount.

For a mount, the firewall blocks the gait events Skyrim sends: `moveStart`,
`moveStop`, `SprintStart`, `SprintStop`, `moveForward`, `moveBackward`,
`BrakeStart`, `BrakeStop`, turns, and canned turns. Its other native events,
such as the mount interaction's and idles, still pass. The network gait
bypasses the firewall, as for every remote actor. A 2 s gait sample per role
logs `Speed` and `HorseSpeedSampled`.

The rider's registration is marked mounted while it is seated. Like furniture
ownership, that lets the firewall admit every native graph event on the rider,
because the mount interaction drives the rider's graph.

## Engine assumptions

These were traced in the 1.7.104 binary, and the calls run only on AE:

- A rider and its mount are linked by an `ExtraInteraction` on the rider,
  holding a `MountInteraction`. `Actor::GetMount` and `IsOnMount` read it.
- The interaction start (ID 41530) passes its quick flag to the
  `MountInteraction` start (ID 40351). Unquick, that start performs the mount
  action through a task on the rider; if the task fails, it stops the
  interaction with mode 2 (ID 41531).
- The dismount (ID 38705) stops the rider's interaction with its mode
  (ID 41531). Mode 0 sets no process flag and asks the `MountInteraction` stop
  (ID 40352) for the animated exit. The interaction is finalized (ID 38696) at
  once only when that exit does not start.
- Skyrim's mount entry (ID 38704) builds a `MountInteraction` only on its
  animated branch. Its quick branch, used by `PutActorOnMountQuick`, restarts
  the rider's existing interaction. `ActorMount::Seat` instead builds one the
  same way, starts it quick, and so skips the mount animation:
  - it allocates 0x38 bytes from Skyrim's `MemoryManager` (`RE::malloc`);
  - it constructs the interaction with ID 40345 (rider, mount);
  - it starts it with ID 41530 (rider, interaction, quick).
- The interaction is reference counted at offset 8. The engine's deleting
  destructor frees it once the last reference is released.
- Removing a seated rider and its mount together is safe, unlike removing them
  during a dismount: seated riding has no paired clip linking their graphs.
  Apart from the AI dismount procedure (ID 39670, mode 0), Skyrim calls the
  dismount with mode 2: from the main loop (ID 36564), an actor update (ID
  37347), and 3D load (ID 40438). That last one drops a rider whose mount is
  gone, so a rider left without its mount is a case the engine handles.

Verified in the first run: the quick start seats a spawned actor on a spawned
mount (`started=true seatedNow=true`). The actor stayed seated for the whole
ride, running the do-nothing package, and followed the mount's translation.

To verify in game, from `latest.log`:

- An animated mount logs `Observed local player mount transition` and
  `Mount sync publishing transition` on the source, and
  `Seating remote player … animated=true started=true` on peers, followed by
  `mount clip started` and, about two seconds later, `mount clip ended`. The
  rider plays the mount clip from its side of the horse, without first walking
  into it, and no later `left its mount locally` follows.
- An animated dismount logs `Starting remote player … dismount … started=true`,
  `finished its dismount`, and `Removed remote player … mount … after actor …
  dismounted`. The actor walks on from where it got off, without a respawn.
- A player killed, or leaving the space, during either clip logs
  `Deferring removal` and then `Removed … after their mount clip ended`, without
  a crash.
- The horse walks at walking pace and gallops only at full speed. After a spawn
  it logs `moving=true` without `sprinting=true`, and few `Corrected remote
  mount graph gait` lines.
- The rider's gait sample reports a `horseSpeedSampled` near the mount's.
- Removing the seated pair on a dismount and on death does not crash
  (`Recycling ... after the player dismounted: seated=true transition=false`).

## Known limitations

- The gallop rarely settles. The measured speed of a galloping source swings
  between about 345 and 615, so the gallop range (start at 560 after 0.3 s,
  stop below 490) is left within about a second, and the mount keeps blending
  in and out of `SprintForward`. The robust fix is to publish the source
  mount's own sprint state, such as its `IsSprinting` graph variable or its
  actor state, rather than inferring the gallop from speed. A cheaper option is
  to hold the gallop until the speed has stayed below its range for a while.

## Acceptance criteria

- A player who mounts appears seated on a horse on every peer. The horse
  follows the player's path and walks, runs, and gallops with its speed.
- A peer who joins, or enters the space, while the player rides sees the actor
  already seated.
- A dismount plays the dismount animation, then removes the horse and leaves
  the actor standing where it got off.
- Mounting plays the mount animation on every peer that has the actor loaded
  when it starts.
- A player killed while riding collapses on peers where it died, and the horse
  disappears.
- A player leaving the world, disconnecting, or timing out removes both the
  actor and the horse.
- The horse cannot be ridden, looted, or harmed locally, and no local actor
  treats it as hostile.
