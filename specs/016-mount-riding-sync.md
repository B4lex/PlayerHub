# Mount Riding

## Status

Implemented. The server side is covered by unit tests. In game, a replayed
rider was seated on its horse, and the horse walked, trotted, and ran with the
recorded speed. Dismounting and death removed the horse without a crash. The
gallop is a known limitation; see [Known limitations](#known-limitations).
Horse jumps are out of scope.

## Goal

When a player rides a mount, peers see the remote actor sitting on a horse that
follows the player's path at a matching gait. The mount exists only while the
player rides and is removed with its rider in every case: dismount, death,
leaving the world, disconnect, timeout, and any retirement of the actor.

## Non-goals

- The mount's appearance. Every remote mount is the vanilla
  `EncHorseSaddledBrown` (`Skyrim.esm:023AB2`), whatever the player rides, so it
  exists in every modpack.
- Mount and dismount animations. Both transitions are instant on peers: a rider
  appears already seated, and a dismount respawns the actor on foot. An earlier
  version played Skyrim's paired `HorseEnter` and `HorseExit` clips, announced by
  a separate `mount:transition` event. The clips link both actors' graphs, so
  every removal, halt, and retry around them needed its own handling, and it was
  dropped.
- Combat on horseback: drawn weapons, attacks, bows, and spells. Attacks queued
  for a riding player are dropped, and hand presentation is not applied.
- Horse jumps, and a mount left standing after a dismount.
- The mount's approach. A peer's horse appears when its player starts to mount.

## Protocol

`player:update` and `player:state` carry `mounted`, a boolean that is true while
the player rides. While it is true, `x`, `y`, `z`, `heading`, `speed`, and
`direction` describe the mount rather than the rider. The receiver drives the
mount from them, and Skyrim carries the rider on the mount. The server validates
`mounted` as a boolean and defaults it to `false` when missing, so older
recordings replay on foot. Every relayed `player:state` includes it, and the
client requires it.

## Source capture

`MountSync::SampleLocalMount` reads the local player's mount once per published
checkpoint (`IsOnMount` and `GetMount`). `MovementSync::SampleLocalMovement`
then samples position, heading, speed, and graph `Direction` from that mount
instead of the player. Speed is still measured against the previous published
sample, so mounting and dismounting cost one speed sample across the offset
between rider and mount. The player is reported mounted for the whole
interaction, including its mount and dismount animations.

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
3. Once both actor and mount are ready, `ActorMount::Seat` seats the actor
   instantly, retrying every 0.5 s. From the first seat on, the mount carries
   the actor: it is never driven on foot again. `ActorRuntime::UpdateRider` only
   keeps its policy, equipment and appearance are still reconciled, and queued
   attacks are dropped.
4. The seat is checked every 0.5 s, since the check looks up the rider's
   interaction. A rider found off its mount is logged and seated again.
5. When the checkpoint no longer rides, the actor and its mount are retired
   together, and the actor respawns on foot. The actor is never taken off the
   mount first (see below). A new seat also waits while either actor is getting
   on or off a mount (`kGettingOnOffMount`).
6. A mount whose handle is lost retires its actor. Both are rebuilt while the
   player still rides.

`ActorLifecycle` owns the mount beside the actor. Every retirement and removal
of the actor also removes the mount in the same pass, rider first, while the
rider is still seated. That covers appearance, coordinate space, fast travel,
revival, a lost actor handle, `player:exit`, `player:disconnect`, and a lost
connection. Mounts are excluded from saves like their riders. When a riding
player dies, `DeathSync` retires the actor and its mount the same way. The actor
respawns on foot, and is then killed where it stands like any actor spawned for
a dead player.

Skyrim's dismount (ID 38705) is never used. Even in mode 1, which sets the
quick-mount process flag, it starts the paired `pa_HorseExit` clip, a
`BSSynchronizedClipGenerator` that links the mount's graph to the rider's.
Removing both actors in the pass that started it crashed on the next graph
event: execution jumped into freed memory from `NotifyAnimationGraph`, with the
mount and its `pa_HorseExit` clip on the stack.

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

The rider is always left to Skyrim's movement update, which carries it, and
its character body is out of every collision while it is on its mount. The
mount skips the movement update, with its body out of collision, since the
network places it (`specs/017-remote-actor-world-isolation.md`).

A seated rider's bone and weapon bodies are on `kBipedNoCC`, which the local
player's proxy controller collides with, and they hang at the mount's flanks,
so the rider blocked the player even with its character body isolated. The
pass-through hook therefore lets the player's movement update ignore every
remote character and skeleton body its phantom (a caching shape phantom)
overlaps, only for that update.

A seated rider's bones also overlap its mount's body, so while the rider is on
its mount, its skeleton bodies have the no-collision bit too. Skyrim rewrote
the rider's character body filter around seating, and a rebuilt 3D brings new
bodies, so the rider update reasserts the bit every half second. It is cleared
once the rider is off the mount or dies. `Remote actor skeleton collision
disabled`, `reasserted`, and `restored` log each change with the number of
bodies changed.

## Engine assumptions

These were traced in the 1.7.104 binary, and the calls run only on AE:

- A rider and its mount are linked by an `ExtraInteraction` on the rider,
  holding a `MountInteraction`. `Actor::GetMount` and `IsOnMount` read it.
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
- A seated rider's character controller stays in the world. After moving a
  ridden mount, its movement update (ID 37350) tries the rider's controller at
  the rider's `actorMountPosition` (ID 78260) to set the mount's
  `kMountPointClear`. The rider's body can therefore overlap the mount's, or
  stand beside it. A proxy controller's rigid body (vfunc 0x10, which proxy
  controllers read from offset 0x5A0) is the body other characters bump, and its
  world collision uses a separate phantom. The contact pass (ID 79130) records
  only one bumped character body per controller (ID 78303), so the pass-through
  hook frees a mover from only one of the two overlapping bodies per update.

Verified in the first run: the quick start seats a spawned actor on a spawned
mount (`started=true seatedNow=true`). The actor stayed seated for the whole
ride, running the do-nothing package, and followed the mount's translation.

To verify in game, from `latest.log`:

- The horse walks at walking pace and gallops only at full speed. After a spawn
  it logs `moving=true` without `sprinting=true`, and few `Corrected remote
  mount graph gait` lines.
- The rider's gait sample reports a `horseSpeedSampled` near the mount's.
- The rider and its horse log `Remote actor character body collision
  disabled`. The local player walks through both, whether on foot or riding.
- The local player walks through a seated rider's legs while it rides.
- A seated rider logs `Remote actor skeleton collision disabled` with a
  non-zero body count.
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
- A dismount removes the horse and leaves the actor standing where the player
  got off.
- A player killed while riding collapses on peers where it died, and the horse
  disappears.
- A player leaving the world, disconnecting, or timing out removes both the
  actor and the horse.
- The horse cannot be ridden, looted, or harmed locally, and no local actor
  treats it as hostile.
