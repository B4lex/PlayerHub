# Native Interactions

## Status

- **Furniture:** implemented for placed `TESFurniture` references on Skyrim AE
  1.7.104 and run in game. Chairs, benches, alchemy tables, forges, and other
  workstations share one native entry path. The engine functions are Address
  Library IDs verified only against 1.7.104; on other runtimes the receiver
  never starts an entry. The native exit ([Exit](#exit)) was checked in game on
  1.7.104 from replay captures of a chair, a bench, and two tables: each played
  its exit idle and then its locomotion, including when the source stood up
  right after sitting down. Beds, crafting stations, and modded exit idles are
  not verified. Plugin-qualified furniture references
  ([Resolution](#source)) are covered by server tests only; peers with
  different load orders have not been run in game.
- **Mounts:** implemented and run in game: a replayed rider was seated on its
  horse, which walked, trotted, and ran with the recorded speed and leaned on
  slopes; dismount and death removed the horse without a crash. The gallop is a
  [known limitation](#known-limitations).
- **Death:** implemented and run in game: a replayed player's deaths, standing
  and seated, collapsed the actor into a ragdoll that the next `player:exit`
  removed. The remaining assumptions and criteria are not checked individually.

The server side of all three is covered by unit tests.

## Scope

Some things the actor does are Skyrim's to perform: sitting in furniture,
riding, and dying. For these the receiver hands the actor to a native engine
path and takes it back afterwards, while no animation, idle, or event name
crosses the wire. Only reconstructable checkpoint state does: `furniture`,
`mounted`, and `dead` ([Network](network.md#checkpoint)).

While an interaction owns the actor, it owns its graph and placement:
translation, heading, locomotion, stance, and attacks wait, the firewall admits
Skyrim's own graph events, and Skyrim places the actor
([Remote actor](remote-actor.md#placement-ownership)). Combat, damage, magic,
targeting, and survival policies stay enforced.

## Components

| Module | Responsibility |
| --- | --- |
| `sync/furniture/FurnitureSync` | Source occupancy sample; receiver target, entry gating, shared-furniture yield |
| `actor/ActorFurniture` | The native sit/sleep lifecycle on the actor |
| `runtime/FurnitureActivationHook` | The local player's furniture activation on its worker thread |
| `sync/mount/MountSync` | Source mount sample; receiver mount spawn, seat, and gait |
| `actor/ActorMount` | Seat construction and checks |
| `sync/death/DeathSync`, `actor/ActorDeath` | Source life state; receiver kill |

## Furniture

A remote player enters, uses, and leaves the same world furniture as its
source, with Skyrim's native entry point, entry idle, furniture idle, and exit
idle. A remote actor must never keep the local player out of furniture nor stack
on a marker another actor uses. Each peer keeps its own world, so several
players can use one reference at once; where they would collide locally, the
remote actor is hidden instead.

### Source

`furniture` is the [form reference](network.md#form-references) of
`Actor::GetOccupiedFurniture()`, which Skyrim sets when the player activates the
furniture, before walking to the entry point and playing the entry idle. It is
published as `null` once the player's sit/sleep
state is WantToStand or WantToWake: the occupancy itself clears only when the
player's exit clip ends (1.1 s for a chair, 3.3 s for a table), and peers start
their own exit clip only when the furniture leaves the checkpoint, so they would
otherwise stay seated for the source's whole exit. The server rejects attacks whose checkpoint uses
furniture, and a furniture checkpoint discards the source's queued attacks. A
reference created at runtime (`FF`) has no form reference and is published as
`null`.

**Resolution.** A raw FormID named different references on peers whose load
orders differ, such as furniture a light plugin adds to a replaced interior, so
the reference carries its plugin name. `FurnitureSync::OnCheckpointAccepted`
resolves it once per change with `FormLookup::ResolveFormID`, and the receiver
works with that local FormID (`RemoteFrame::furnitureRefID`). The ID is computed
from the plugin's local compile index without looking the reference up, so a
reference in a cell that has not loaded yet still resolves, and entry waits for
it to load. A light plugin's `formId` above `0xFFF` is refused rather than
carried into the compile index. A reference whose plugin is not loaded here
resolves to zero and is logged once per change: the actor stays standing at its
source's seat, while combat still treats the source as seated.

### Entry

`TESFurniture::Activate` refuses every activator but the player, so the
receiver uses the NPC path. It waits until:

- the reference resolves to a loaded `TESFurniture` in the actor's space;
- the actor has been ready for 1.5 s. An actor created in the pass the local
  cell finished loading has not run one update with its do-nothing package,
  equipment, or a settled AI process; an entry there is accepted, torn down by
  the engine within one pass of `IsSitting`, and leaves the marker reserved. An
  actor resuming its source's furniture (below) waits only one update after
  readiness: it has no entry idle to tear down, and waiting would leave it
  standing on the seat;
- its locomotion has settled and no attack, weapon, presentation, or recovery
  transition is pending. A completed weapon clip counts as settled while the
  target is idle (after a sheathe, once the default state was forced), though
  its locomotion replay stays armed for later movement;
- the actor is within 24 units of the latest checkpoint;
- the furniture has an available marker (ID 20162), and one of its markers is
  within 96 units (2D). The distance is to the nearest marker, not the probe's:
  the probe returns the first free index while the native entry picks its own,
  so on a bench the probe's marker could be at the far end and held the entry
  back for as long as the source sat;
- the local player does not occupy that reference. The probe does not report the
  player's own occupancy, and an entry there records occupancy the engine takes
  back on the next pass, restarting the entry idle while the player sits. Such an
  actor is hidden instead.

The settled-locomotion, transition, checkpoint-distance, and marker-distance
gates each log once when they start holding, with the distances. The receiver
does not wait for the source to settle on the marker, which would delay the actor
by the source's whole entry idle; the native entry aligns it.

The receiver snaps the actor to the checkpoint transform and calls the entry NPC
Sit procedures use (ID 37330): it records the occupied reference, chooses an
entry point (ID 39923: entry idle, path point, angle, marker), and reserves the
marker. Without an entry point the occupancy is cleared and entry retried after
1 s. With a weapon drawn, ID 39923 only requests a sheathe and returns without
an entry point; Skyrim's Sit procedure (ID 39648) selects again while the marker
is unset, so the entry is repeated once sheathed, every second, and a fresh
entry is attempted when none is chosen within 10 s.

ID 37330 reports success before its occupancy is observable, and sometimes
records none. An entry is confirmed on a later pass, when the actor is seen
occupying the target; an unconfirmed one is retried after 1.5 s rather than the
10 s allowed for an entry that sat and then dropped.

**Resuming.** An actor spawned while its source already uses furniture resumes
it seated, since the source's entry played before the actor existed: a peer
coming into view seated, an actor rebuilt after an appearance change or fast
travel, or one returning after being hidden. Its first entry into that reference
passes the same gates (with the shorter settle), then follows the path Skyrim
uses to reseat an actor whose 3D loads in furniture (ID 40438): record the
occupancy (ID 39955), choose an entry point (ID 39923), and pass the chosen
marker to the quick entry (ID 37324), which reserves it, plays the
`Idle*EnterInstant` record, sets IsSitting or IsSleeping, and snaps the actor onto
the marker (ID 37326, whose result it returns). The quick entry takes the marker
as a third argument; an attempt without it failed to seat the actor. A seated
state counts as success even if the snap failed. A refusal releases what it
recorded and falls back to the transition entry. Once the source moves to
another target, resuming no longer applies, so a later entry plays its
transition.

### Use

While occupied, the receiver runs Skyrim's per-frame sit/sleep state machine (ID
37332 → 39861) itself, because Skyrim reaches it only from package procedure
branches the do-nothing package never takes. It plays the entry idle (WantToSit
→ WaitingForSitAnim), and the graph completes IsSitting. The actor stays on its
do-nothing package, and any other package is taken back as outside furniture;
alias and scene packages are refused
([Remote actor](remote-actor.md#package-takeover)).

### Exit

When the checkpoint changes to zero or another reference:

- **Seated actor:** from IsSitting or IsSleeping, the receiver calls the get-up
  NPC package procedures run (ID 39865), which the do-nothing package never
  reaches. It performs `ActionActivate` (AACT `00013009`) on the occupied
  furniture, so Skyrim chooses the exit idle from the receiver's own idle
  records, then sets WantToStand or WantToWake. Exit idles are not the entry
  idles' siblings; they sit under `ActivateRootChar` behind sit/sleep conditions:
  `ExitChair` (`GetSitting` 3–4) with Front, Left, Right, and Back variants chosen
  by `IsFurnitureEntryType`, each crafting root's `…Exit` idle, and `ExitBedRoot`
  (`GetSleeping` 3–4). Setting WantToStand directly skips that selection, and
  `AIProcess::PlayIdle` performs `ActionIdle`, whose tree holds none of them.
  Each peer plays its own exit idle, vanilla or modded, at its own length, in
  full.

  The engine releases the furniture when the exit clip ends: the graph emits
  `IdleFurnitureExit`, `tailMTLocomotion`, `IdleStop`, and `idleChairGetUp`, and
  `ChairFurnitureExitHandler`/`BedFurnitureExitHandler` return the actor to
  Normal and clear the occupancy (ID 39912). Captures measured about 1.1 s for a
  chair and 3.3 s for a table. A chair's clip has ended by then. A table's has
  not: `bAnimationDriven` stays set and a second `idleChairGetUp` follows about
  0.2 s later, and locomotion dispatched in between is accepted but never
  played. The exit therefore completes once the furniture is released and
  `bAnimationDriven` is clear, or 2 s after the release. In both cases the clip
  itself has ended about 0.2 s earlier, when the graph emits `IdleFurnitureExit`
  and leaves the furniture node; the rest is a get-up settle. If the actor is
  then more than 32 units from its network target, the source has moved on and
  the settle is cut: the receiver releases the furniture itself, sets Normal
  (the engine's exit handler then ignores the actor), forces
  `IdleForceDefaultState`, and completes the exit in that pass. An exit in progress
  runs to completion before the actor enters any furniture again, including the
  seat its source has just returned to.
- **Forced exit:** when the get-up leaves the actor in neither WantToStand nor
  WantToWake, or the furniture is not released within 5 s, the receiver sets
  WantToStand/WantToWake and sends `IdleFurnitureExit`, which leaves the
  furniture node without an exit clip, retrying the state once per second. The
  actor returns to Normal in about 200 ms.

  No get-up package is installed. `InitiateGetUpPackage` installs a run-once
  package whose procedure can never finish once the actor has left the
  furniture. It outranks and survives the do-nothing package, so combat
  sanitization retook it every pass for the rest of the session, and the engine
  moved the actor off its marker in between.
- **Entry idle still playing:** a source stands only once seated, so an exit
  that arrives while the requested entry is in WaitingForSitAnim means the entry
  here started later or runs longer. The receiver lets the entry finish and then
  starts the native exit, for at most 10 s. Cancelling the entry instead snapped
  the actor upright when its source stood up right after sitting down.
- **Entry not yet seated:** otherwise there is no native exit, so the receiver
  releases marker reservations, clears the occupancy, returns the state to
  Normal, and forces `IdleForceDefaultState`. The event matters: the engine bookkeeping is not
  what plays the entry idle, and left alone the idle reaches its seated node a
  second later and sets IsSitting on an actor that occupies nothing. Occupancy
  taken on a reference never requested resolves the same way; if it survives 3 s,
  the interaction is abandoned and retried from entry.
- **Seated without occupancy:** a sit or sleep state other than Normal is unwound
  through the forced exit even with no occupancy, request, or target left, since
  the get-up needs the occupied reference. Nothing else observes that state, and
  the actor would hold the pose until something reset its graph.
- **Completion:** once the actor is Normal and, unless a native exit released
  it, unoccupied, and a native exit's clip no longer drives it, the receiver
  clears any occupancy left behind, restores the do-nothing package, and
  rebuilds the latest locomotion checkpoint.
- **Stuck exit:** after 10 s the receiver clears the occupancy, forces
  `IdleForceDefaultState`, and restores the do-nothing package. Target changes
  during an exit do not reset this timer.

Changing directly between references exits the old one first. If Skyrim drops a
started entry, ownership is held at most 10 s before the request is released and
retried.

### Shared furniture

When a remote actor would block the local player or share a marker, it is hidden
locally: retired like any respawn (occupancy and reservations released,
reference disabled and deleted) while its source keeps using the furniture. A
hidden actor is not respawned while its newest checkpoint still names that
reference.

**Local player activation.** Skyrim runs the player's furniture activation
(`TESFurniture::Activate`, vfunc 0x37) on a worker thread. A hook there reads,
under the registry lock, the furniture each remote actor holds (occupied or
requested), which every furniture pass publishes. If one holds the reference,
the hook refuses the activation, so Skyrim shows no "in use" message, and
defers it. Either way the activation is queued. At the start of the next pass,
before the local checkpoint is sampled, each queued activation resolves:

- if the local player already occupies or reserves the reference, it was
  accepted and nothing is done;
- otherwise every remote actor holding it (occupied, requested, or reserved)
  while the probe (ID 20162) finds no free marker for the player is hidden;
- if an actor was hidden or the activation deferred, the reference is activated
  again through `ActivateRef` with default processing only, since its scripts
  already ran.

On a bench with a free marker nobody is hidden, and the player sits beside the
actor.

**Remote entry.** Before its actor is updated, a remote actor is hidden if the
local player occupies the checkpoint's reference and the actor does not already
hold it (an actor seated beside the player on a bench keeps its seat), or if its
last entry attempt found no free marker and the reference carries a reservation
by another live, enabled actor, local or remote. The later actor yields. A probe
failure without such a reservation (stale, or a runtime other than 1.7.104) keeps
the 1 s retry. The flag comes from the entry attempt, so the actor is hidden one
pass later.

**Return.** A hidden actor spawns again when its checkpoint leaves the reference
(standing), or when the reference is vacated locally: the player neither
occupies nor reserves it and the probe finds a free marker for the player (the
actor then resumes seated; this needs 1.7.104). If the player sits back down
before the returned actor entered, it is hidden again.

Logs: `Hiding remote player actor ... reason=`,
`Restoring remote player actor ... was vacated`,
`Observed local furniture activation`, and
`Reactivated furniture for the local player`.

### Ordering

- An already playing attack finishes before furniture entry; furniture exit
  finishes before newer locomotion or attacks apply.
- Respawns and coordinate-space changes discard local furniture progress and
  reconstruct from the newest checkpoint. Historical enter and exit transitions
  are not replayed.
- Shared-furniture resolution runs after coordinate-space and fast-travel
  retirement and before the actor is updated; local activations are resolved
  before the local checkpoint is sampled.

## Mounts

A riding player appears on peers seated on a horse that follows its path at a
matching gait. The mount exists only while the player rides and is removed with
its rider in every case.

### Source

`MountSync::SampleLocalMount` reads the mount once per checkpoint (`IsOnMount`,
`GetMount`), and the checkpoint's position, heading, speed, `Direction`,
swimming, and `pitch` are then sampled from the mount. Speed is still measured
against the previous sample, so mounting and dismounting cost one speed sample
across the rider-to-mount offset. The player counts as mounted for the whole
interaction, including its mount and dismount animations.

### Receiver

`MountSync::SyncMount` runs after the death and stranded-actor checks and before
every on-foot stage:

1. While the newest checkpoint rides and the player is alive, a mount is spawned
   at the checkpoint through `ActorSpawner`, so it is registered like a remote
   actor: survival, combat, magic, hostility, and collision isolation, and
   blocked activation, so it cannot be ridden or stolen. Every remote mount is
   the vanilla `EncHorseSaddledBrown` (`Skyrim.esm:023AB2`), which exists in every
   modpack.
2. The mount takes each checkpoint as its target. Once ready,
   `ActorRuntime::UpdateMount` reasserts its policy, drives its gait, and
   translates it with the actor translation code.
3. Once actor and mount are ready, `ActorMount::Seat` seats the actor instantly,
   retrying every 0.5 s. From then on the mount carries it: it is never driven on
   foot again. `ActorRuntime::UpdateRider` keeps its policy, equipment and
   appearance are still reconciled, and queued attacks are dropped.
4. The seat is checked every 0.5 s, since the check looks up the rider's
   interaction; a rider found off its mount is logged and seated again. A seat
   also waits while either actor is getting on or off (`kGettingOnOffMount`).
5. When the checkpoint no longer rides, actor and mount are retired together and
   the actor respawns on foot, never taken off the mount first.
6. A mount whose handle is lost retires its actor; both are rebuilt while the
   player rides.

`ActorLifecycle` owns the mount beside the actor. Every retirement and removal
removes the mount in the same pass, rider first, while the rider is still
seated. When a riding player dies, the pair is retired the same way, and the
actor respawns on foot and is killed where it stands.

Transitions are instant: a rider appears seated, and a dismount respawns the
actor on foot. An earlier version played Skyrim's paired `HorseEnter` and
`HorseExit` clips, announced by a `mount:transition` event; the clips link both
actors' graphs, so every removal, halt, and retry around them needed its own
handling, and it was dropped. Skyrim's dismount (ID 38705) is never used: even in
mode 1, which sets the quick-mount flag, it starts the paired `pa_HorseExit`
clip, a `BSSynchronizedClipGenerator` that links the graphs, and removing both
actors in the pass that started it crashed in `NotifyAnimationGraph` with the
mount and its clip on the stack.

### Gait

The mount's gait comes from the checkpoint speed. Vanilla horse movement types
are walk 125, run 450 (`Horse_Default_MT`), and sprint 600 (`Horse_Sprint_MT`).

- Below 20 the mount is idle (`moveStop`). The gallop (`SprintStart`,
  `SprintStop`, `IsSprinting`) starts only after the speed stays at 560 or more
  for 0.3 s, so a single relocation sample cannot start it, and stops below 490.
  `SprintStop` is sent before `moveStop`.
- `Speed` and `SpeedSampled` take the checkpoint speed once per new checkpoint,
  and `Direction` stays zero.
- An edge records the request whether or not the graph accepts it. Every 0.25 s
  the graph's `IsMoving` and its gallop (the state tags `iState` with
  `iState_HorseSprint`) are compared with the request, and a mismatch sends
  `SprintStop`, `moveStart`, `moveStop`, or `SprintStart`, in that priority
  (`Corrected remote … graph gait`).

Why: the first checkpoint after a mount spawns carries the relocation onto it
(speeds of 1176 and 2442), which started a gallop; `moveStop` then left
locomotion without leaving the gallop state inside it, and every later
`moveStart` resumed `SprintForward`, so the blend never ran and `SprintStart`
looked rejected.

`HorseBehavior.hkx` (vanilla), read from its packfile: `moveStart` enters
`LocomotionState` and `moveStop` leaves it; `ForwardLocomotionBlend_Horse` blends
on `HorseSpeedSampled` in world units (slow walk 5, walk 125, slow trot 214, trot
304, run 450); the sprint behavior is active while `IsSprinting` is set; a
`BSSpeedSamplerModifier` writes `HorseSpeedSampled` from `Speed`, `iState`, and
`Direction` through a global speed sampler Skyrim installs, otherwise copying
`Speed`; there is no `SpeedSampled`, and `fMinSpeed` is 5.

Skyrim polls `Speed` through `ActorSpeedChannel` from the actor's own movement,
which is zero for a mount moved by its transform, so `Speed` fell back to zero
after each write and the gait played in place. A remote mount's channel reports
the network speed; the registry counts mounts, so while no one rides, the hook
returns for local actors without the lock.

The rider has its own `horsebehavior.hkx` (under `actors\character`) with the
same blend, driven by its own `Speed`, `Direction`, and `HorseSpeedSampled`. Its
`Speed` and `IsSprinting` follow the mount's speed and its `Speed` channel
reports it; its `Direction` channel reports zero, since its last on-foot value
is stale; seating clears its jump state. The rider's graph rejects `moveStart`
and `moveStop` (over 2000 times in one run), so it gets no locomotion events; it
takes every native event, which reach it from its mount.

For a mount, the firewall blocks the gait events Skyrim sends (`moveStart`,
`moveStop`, `SprintStart`, `SprintStop`, `moveForward`, `moveBackward`,
`BrakeStart`, `BrakeStop`, turns, and canned turns) and passes its other native
events. The rider's registration is marked mounted while seated, which admits
every native event on the rider, since the mount interaction drives its graph. A
2 s gait sample per role logs `Speed` and `HorseSpeedSampled`.

### Terrain pitch

A horse leans to its slope: Skyrim keeps the tilt in the controller's
`pitchAngle` and `rollAngle`, and every frame the actor's 3D update (ID 37364)
builds the body rotation from them and the heading; a seated rider is placed
from its mount's 3D and tilts with it. The angles come from the ground only in
the movement update, which network placement skips, so a remote mount stayed
level.

The source samples its carrier's pitch per checkpoint; its horse is simulated
normally, so the pitch follows its own ground. On a peer the mount update eases
its controller's pitch toward the checkpoint's with a 0.1 s time constant,
hiding the 50 ms steps. Only a controller with `kCanPitch` is pitched, as in
Skyrim, where it comes from the race flag `kTiltFrontBack` (ID 78305). Roll is
not synchronized: the vanilla horse does not roll, and the source's roll stayed
zero on every slope.

Traced in the 1.7.104 binary: the biped orientation controller (vfunc 8, ID
79327) moves each angle toward the support normal, clamped to 47°, by `pitchMult`
every physics step; pathing (ID 38308) and placement (ID 37314) set them from the
navmesh normal. `Actor::SetPosition` (ID 37309) and `SetHeading` (ID 37230) leave
them alone. The move-finish sink (ID 37996) overwrites them from the body's
rotation, but `kNoSim` suppresses it for a network-placed actor.

### Collision

The rider is always left to Skyrim's movement update, which carries it, and its
character body is out of every collision while on its mount; the mount is
network-placed with its body out of collision
([Remote actor](remote-actor.md#world-isolation)).

A seated rider's bone and weapon bodies are on `kBipedNoCC`, which the local
player's proxy controller collides with, and hang at the mount's flanks, so the
rider blocked the player even with its character body isolated. The
pass-through hook therefore lets the player's movement update ignore every
remote character and skeleton body its phantom (a caching shape phantom)
overlaps, for that update only.

A seated rider's bones also overlap its mount's body, so while on its mount its
skeleton bodies get the no-collision bit too. Skyrim rewrites the rider's filter
around seating and a rebuilt 3D brings new bodies, so the rider update reasserts
it every 0.5 s; it is cleared once the rider is off the mount or dead.
`Remote actor skeleton collision disabled`, `reasserted`, and `restored` log each change
with the body count.

## Death

When a player dies, peers see the actor die like a vanilla NPC: it collapses
into a ragdoll where physics leaves it. Vanilla death ends in a load (the last
save or the main menu), whose `player:exit` removes the body, so peers see it for
the few seconds of the death camera.

### Source

`DeathSync::SampleLocalDeath` reads the player's `ACTOR_LIFE_STATE` once per
checkpoint. `kDying` and `kDead` publish `dead`; every other state, including
`kBleedout` and `kEssentialDown`, publishes alive. Checkpoints keep flowing while
dead, until the load's `player:exit`. `dead` is checkpoint state, not an action,
so a peer that joins, respawns the actor, or re-enters the space during the death
camera still sees a body.

### Receiver

`DeathSync::SyncDeath` runs after the coordinate-space checks and before every
stage that drives the actor:

1. A live actor whose newest checkpoint is dead is killed once ready (3D,
   controller, graph), so an actor spawned for a dead player stands for the few
   passes of its initialization and then collapses. Queued attacks are dropped.
2. A killed actor is left alone: movement, animation, furniture, equipment,
   combat, and the stranded check are skipped. Only its handle is watched (a
   lost handle respawns it) and its appearance presentation kept.
3. A killed actor whose checkpoint is alive again (console `resurrect`, a
   reviving mod) is retired and respawned standing.

`ActorDeath::Kill` hands the actor to Skyrim:

- Its furniture is released first, as on retirement, including the hold
  published to the local player's activation hook.
- `RemoteCollision` releases placement (`owner=death`).
- `ActorRegistry::SetDead` marks the registration dead: the `KillImpl`,
  `KillDying`, `HandleHealthDamage`, and health `ModActorValue` guards let calls
  through, the update hook stops reasserting the survival policy (which would
  resurrect it), and the firewall admits every graph event.
- `kEssential` and `kProtected` are cleared.
- `Actor::KillImpl(nullptr, 0, false, true)` kills it with no attacker and no
  death event, so no local quest, script, crime, or statistic observes it, and
  ragdolls it immediately.

The registration is per reference, so a respawned actor starts protected again.

## Engine assumptions

Mounts, traced in the 1.7.104 binary (AE only):

1. A rider and mount are linked by an `ExtraInteraction` on the rider holding a
   `MountInteraction`; `Actor::GetMount` and `IsOnMount` read it.
2. Skyrim's mount entry (ID 38704) builds a `MountInteraction` only on its
   animated branch; its quick branch (`PutActorOnMountQuick`) restarts the
   rider's existing one. `ActorMount::Seat` allocates 0x38 bytes from Skyrim's
   `MemoryManager` (`RE::malloc`), constructs the interaction with ID 40345
   (rider, mount), and starts it quick with ID 41530, skipping the animation.
   The interaction is reference counted at offset 8 and freed by the engine's
   deleting destructor. Verified in game: the quick start seats a spawned actor
   (`started=true seatedNow=true`), which stayed seated on the do-nothing
   package for the whole ride.
3. Removing a seated pair together is safe: seated riding has no paired clip.
   Besides the AI dismount procedure (ID 39670, mode 0), Skyrim dismounts with
   mode 2 from the main loop (ID 36564), an actor update (ID 37347), and 3D load
   (ID 40438), which drops a rider whose mount is gone, so a rider without a
   mount is a case the engine handles.
4. A seated rider's controller stays in the world. After moving a ridden mount,
   its movement update (ID 37350) tries the rider's controller at
   `actorMountPosition` (ID 78260) to set `kMountPointClear`, so the rider's body
   can overlap the mount's or stand beside it. A proxy controller's rigid body
   (vfunc 0x10, read from offset 0x5A0) is what other characters bump, and its
   world collision is a separate phantom. The contact pass (ID 79130) records one
   bumped body per controller (ID 78303), so the pass-through hook frees a mover
   from only one of two overlapping bodies per update.

Death:

5. `KillImpl` with `a_sendEvent = false` still performs the full death: life
   state, ragdoll, and stopped AI. Clearing `kEssential` is enough to kill an
   actor spawned from a remote player's base.
6. A dead temporary reference keeps a valid handle while its cell stays loaded;
   if Skyrim cleans it up, the body is respawned and killed again.
7. The player's life state passes `kDying`/`kDead` on a vanilla death before the
   load opens.

Furniture exit, traced in the 1.7.104 binary:

8. ID 39865 is `bool(AIProcess*, Actor*)`. From IsSitting or IsSleeping it
   processes a `TESActionData` (priority kTry, source the actor, action
   `ActionActivate`, target the occupied furniture) and, if accepted, sets
   WantToStand or WantToWake through ID 39912, keeping the furniture handle and
   marker. From WantToStand or WantToWake it does nothing. Verified in game for
   chairs and tables.
9. `ChairFurnitureExitHandler` and `BedFurnitureExitHandler` return an actor in
   a sit or sleep state to Normal and clear its occupancy (ID 39912 with no
   furniture). In game this happened at the end of the exit clip, not its
   start; for a table, about 0.2 s before `bAnimationDriven` cleared.

## Acceptance criteria

Furniture:

1. Entering a resolvable reference makes the actor use the same reference with
   its native entry idle, starting close to the source's entry. Leaving plays the
   native exit and returns to the latest locomotion.
2. Dropped snapshots and late join converge to the newest reference or none; a
   player already in furniture appears in it without an entry idle on the first
   update after its actor is ready.
3. Furniture use pauses translation and cannot overlap an attack, jump, or weapon
   transition. Unresolved, non-furniture, and cross-space references, missing
   markers, and stuck transitions are bounded, logged, and retried.
4. The local player can use furniture a remote actor holds without an "in use"
   message; the actor is hidden while its source stays or the player keeps the
   last free marker, and returns seated once it is vacated. Two actors never
   stack on one marker.
5. Recordings without the field, including ones made with the load-order
   `furnitureRefId`, replay without furniture. A remote actor still uses
   furniture after being hit.
6. Peers whose load orders differ but that both load the furniture's plugin
   see each other use it, including furniture a light plugin adds.

Mounts:

7. A mounting player appears seated on every peer, including for peers who join
   or enter the space while it rides. The horse follows its path and walks,
   runs, and gallops with its speed (`moving=true` without `sprinting=true`
   after a spawn, few `Corrected remote mount graph gait` lines), and the rider's
   `horseSpeedSampled` is near the mount's.
8. A dismount removes the horse and leaves the actor standing where the player
   got off. Death, leaving the world, disconnecting, or timing out removes both
   without a crash
   (`Recycling ... after the player dismounted: seated=true transition=false`).
9. The horse cannot be ridden, looted, or harmed locally, and no local actor
   treats it as hostile. The local player walks through a rider and its horse,
   on foot or riding (`Remote actor character body collision disabled`), and a
   seated rider logs `Remote actor skeleton collision disabled` with a non-zero
   count.
10. A horse on a slope leans with it and levels out on flat ground, and its rider
    leans with it (`pitch` near `targetPitch`).

Death:

11. A player killed by an enemy collapses into a ragdoll on every peer, and the
    body disappears when its game loads. A peer entering the cell during the
    death camera sees it collapse where the player died.
12. The body cannot be looted, and the local player gains no kill, bounty, or
    quest progress.
13. A player revived with `resurrect` stands up again as a fresh actor. A player
    in bleedout (essential) stays standing.

## Known limitations

- The gallop rarely settles. A galloping source's measured speed swings between
  about 345 and 615, so the gallop range is left within about a second and the
  mount keeps blending in and out of `SprintForward`. The robust fix is to
  publish the source mount's own sprint state (its `IsSprinting` or actor state)
  rather than infer it from speed; a cheaper one is to hold the gallop until the
  speed stays below its range for a while.
- Furniture: a native exit plays its full clip while the source may already
  be walking away (3.3 s for a table). The actor then catches up by running
  along the network path, or snaps to the source if the source is standing
  still by then.
- Furniture: crafting menus, recipes, ingredients, output, skill progression,
  sleep and wait UI, ownership and crime, arbitrary activators, doors,
  containers, scripted furniture, paired idles, and killmoves are not
  synchronized; there is no global reservation across clients; a peer without
  the furniture's plugin shows the actor standing, with no substitute furniture
  or seated idle.
- Mounts: the source's mount appearance, mount and dismount animations, combat
  on horseback (drawn weapons, attacks, bows, spells), horse jumps, a mount left
  standing after a dismount, and the mount's approach are not shown.
- Death: the killer, kill moves, death animations (the actor always goes straight
  to ragdoll), where the body comes to rest (each peer simulates its own), and
  bleedout are not shown.
