# Native Interactions

## Status

The dragon's `PitchDelta` [flight channel](#dragon-flight) is unconfirmed, so
its bending may not match the source's. Retried flight events have not run in
game.

## Scope

Some things the actor does are Skyrim's to perform: sitting in furniture,
riding, and dying. For these the receiver hands the actor to a native engine
path and takes it back afterwards. No animation, idle, or event name of the
interaction crosses the wire, only reconstructable checkpoint state:
`furniture`, `mounted`, `flyingMount` with the mount's fly state, and `dead`
([Network](network.md#checkpoint-and-context)).

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
| `actor/ActorMount` | Mount base, seat construction and checks, gait |
| `actor/ActorFlight` | A remote dragon's flight graph, driven from the source's fly state |
| `sync/death/DeathSync`, `actor/ActorDeath` | Source life state; receiver kill |

## Furniture

A remote player enters, uses, and leaves the same world furniture as its
source, with Skyrim's native entry point, entry, furniture, and exit idles. A
remote actor must never keep the local player out of furniture nor stack on a
marker another actor uses. Several players can use one reference at once, each
in their own world; where they would collide locally, the remote actor is
hidden instead.

### Source

`furniture` is the [form reference](network.md#form-references) of
`Actor::GetOccupiedFurniture()`, which Skyrim sets on activation, before the
walk to the entry point and the entry idle. It is published as none once the
sit/sleep state is WantToStand or WantToWake: the occupancy clears only when
the exit clip ends (1.1 s for a chair, 3.3 s for a table), and peers start
their own exit only when the furniture leaves the checkpoint. Furniture travels
in the `Context`. The server rejects attacks whose context has furniture, and a
furniture checkpoint discards queued attacks. A runtime reference (`FF`) has no
form reference and is published as none. The idle events Skyrim sends around
furniture are not relayed as idles
([Idle presentation](idle-presentation.md#capture)).

**Resolution.** The reference carries its plugin name, since a raw FormID names
different references on peers with different load orders.
`FurnitureSync::OnCheckpointPlayed` resolves it once per change with
`FormLookup::ResolveFormID` into `RemoteFrame::furnitureRefID`, computed from
the plugin's compile index without a lookup, so a reference in a cell not yet
loaded resolves and entry waits for it. A light plugin's `formId` above
`0xFFF` is refused. A reference whose plugin is not loaded resolves to zero and
is logged once per change: the actor stands at its source's seat, while combat
still treats the source as seated.

### Entry

`TESFurniture::Activate` refuses every activator but the player, so the
receiver uses the NPC path. It waits until:

- the reference resolves to a loaded `TESFurniture` in the actor's space;
- the actor has been ready for 1.5 s, since an entry before its first update
  with the do-nothing package, equipment, and a settled AI process is torn
  down by the engine and leaves the marker reserved. A resuming actor (below)
  waits only one update, since it has no entry idle to tear down;
- its locomotion has settled and no attack, weapon, presentation, idle, or
  recovery transition is pending (furniture releases an idle,
  [Idle presentation](idle-presentation.md#release)). A completed weapon clip
  counts as settled while the target is idle;
- the actor is within 24 units of its pose;
- the furniture has an available marker (ID 20162) and any of its markers is
  within 96 units (2D); the nearest marker counts, since the native entry picks
  its own, not the probe's first free index;
- the local player does not occupy the reference, which the probe does not
  report and the engine would take back on the next pass, restarting the entry
  idle; such an actor is hidden instead.

The settled-locomotion, transition, checkpoint-distance, and marker-distance
gates each log once when they start holding. The receiver does not wait for the
source to settle on the marker; the native entry aligns the actor.

The receiver snaps the actor to the checkpoint transform and calls the entry
NPC Sit procedures use (ID 37330), which records the occupied reference, picks
an entry point (ID 39923: entry idle, path point, angle, marker), and reserves
the marker. Without an entry point the occupancy is cleared and entry retried
after 1 s. With a weapon drawn, ID 39923 only requests a sheathe, so the entry
repeats every second once sheathed, and afresh when none is chosen within 10 s.
Before each repeat the receiver releases its reservations on the reference and
returns the actor to Normal, since Skyrim's sit/sleep update may already have
set WantToSit and reserved the unset marker
([assumption 10](#engine-assumptions)), which made the later entry sit in the
air.

ID 37330 reports success before its occupancy is observable, and sometimes
records none, so an entry is confirmed when the actor is seen occupying the
target on a later pass; an unconfirmed one is retried after 1.5 s.

**Resuming.** An actor spawned while its source already uses furniture resumes
it seated: a peer coming into view seated, an actor rebuilt after an appearance
change or fast travel, or one returning from being hidden. Its first entry into
that reference passes the same gates, then follows Skyrim's reseat path for an
actor whose 3D loads in furniture (ID 40438): record the occupancy (ID 39955),
choose an entry point (ID 39923), and pass the marker to the quick entry
(ID 37324, which needs it as third argument), which reserves it, plays the
`Idle*EnterInstant` record, sets IsSitting or IsSleeping, and snaps the actor
onto the marker (ID 37326). A seated state counts as success even if the snap
failed; a refusal releases what it recorded and falls back to the transition
entry. Once the source moves to another target, a later entry plays its
transition.

### Use

While occupied, the receiver runs Skyrim's per-frame sit/sleep state machine
(ID 37332 → 39861) itself, since Skyrim reaches it only from package branches
the do-nothing package never takes. It plays the entry idle (WantToSit →
WaitingForSitAnim), and the graph completes IsSitting. Any package other than
do-nothing is taken back; alias and scene packages are refused
([Remote actor](remote-actor.md#package-takeover)).

### Exit

When the checkpoint changes to zero or another reference:

- **Seated actor:** from IsSitting or IsSleeping, the receiver calls the get-up
  procedure (ID 39865), which performs `ActionActivate` (AACT `00013009`) on the
  furniture, so Skyrim picks the exit idle from the receiver's own idle records
  (`ExitChair` variants by `IsFurnitureEntryType`, each crafting root's
  `…Exit`, `ExitBedRoot`, under `ActivateRootChar`), then sets WantToStand or
  WantToWake. Setting WantToStand directly would skip that selection, and
  `AIProcess::PlayIdle`'s `ActionIdle` tree holds none of them. Each peer plays
  its own exit idle in full.

  The engine releases the furniture when the exit clip ends:
  `ChairFurnitureExitHandler`/`BedFurnitureExitHandler` return the actor to
  Normal and clear the occupancy (ID 39912). A table's clip still drives the
  actor for about 0.2 s after (`bAnimationDriven` set, a second
  `idleChairGetUp`), and locomotion sent then is accepted but never played, so
  the exit completes once the furniture is released and `bAnimationDriven` is
  clear, or 2 s after the release. If the actor is then more than 32 units from
  its network target, the source has moved on and the get-up settle is cut: the
  receiver releases the furniture, sets Normal, forces
  `IdleForceDefaultState`, and completes the exit at once. An exit runs to
  completion before the actor enters any furniture again.
- **Forced exit:** when the get-up leaves the actor in neither WantToStand nor
  WantToWake, or the furniture is not released within 5 s of WantToStand or
  12 s of WantToWake, the receiver sets the state and sends
  `IdleFurnitureExit`, which leaves the furniture node without an exit clip,
  retrying once per second. No get-up package is installed:
  `InitiateGetUpPackage`'s run-once package can never finish once the actor has
  left the furniture, and it outranks and survives the do-nothing package.
- **Entry idle still playing:** an exit during WaitingForSitAnim lets the entry
  finish, then starts the native exit, for at most 10 s; cancelling the entry
  snapped the actor upright.
- **Entry not yet seated:** the receiver releases marker reservations, clears
  the occupancy, returns to Normal, and forces `IdleForceDefaultState`, without
  which the entry idle would still reach its seated node and set IsSitting on
  an actor occupying nothing. Occupancy on a reference never requested resolves
  the same way; if it survives 3 s the interaction is abandoned and retried.
- **Seated without occupancy:** a sit or sleep state other than Normal with no
  occupancy, request, or target is unwound through the forced exit, unless a
  network [idle](idle-presentation.md#ownership) owns the graph (such idles set
  the sitting state themselves).
- **Completion:** once the actor is Normal, unoccupied unless a native exit
  released it, and no longer driven by an exit clip, the receiver clears any
  leftover occupancy, restores the do-nothing package, and rebuilds the latest
  locomotion checkpoint.
- **Stuck exit:** 10 s after the exit starts, or after a native exit gives way
  to the forced exit, the receiver clears the occupancy, forces
  `IdleForceDefaultState`, and restores the do-nothing package. Target changes
  during an exit do not reset this timer.

Changing directly between references exits the old one first. A started entry
Skyrim drops holds ownership at most 10 s before it is released and retried.

### Shared furniture

When a remote actor would block the local player or share a marker, it is
hidden locally: retired like any respawn while its source keeps using the
furniture, and not respawned while its checkpoint still names that reference.
Its player stays audible from the seat ([Voice chat](voice-chat.md#playback)).

**Local player activation.** Skyrim runs the player's furniture activation
(`TESFurniture::Activate`, vfunc 0x37) on a worker thread. A hook there reads,
under the registry lock, the furniture each remote actor holds (occupied or
requested, published by every furniture pass). If one holds the reference, the
hook refuses the activation, so no "in use" message shows, and defers it. Either
way the activation is queued and resolves at the start of the next pass,
before the local checkpoint is sampled:

- if the player already occupies or reserves the reference, nothing is done;
- otherwise every remote actor holding it is hidden while the probe (ID 20162)
  finds no free marker for the player;
- if an actor was hidden or the activation deferred, the reference is activated
  again through `ActivateRef` with default processing only, since its scripts
  already ran.

On a bench with a free marker nobody is hidden.

**Remote entry.** Before its update, a remote actor is hidden if the local
player occupies the checkpoint's reference and the actor does not already hold
it, or if its last entry attempt found no free marker and another live,
enabled actor reserves the reference. The later actor yields; a probe failure
without such a reservation keeps the 1 s retry.

**Return.** A hidden actor spawns again when its checkpoint leaves the
reference, or when the reference is vacated locally (the player neither
occupies nor reserves it and the probe finds a free marker); it then resumes
seated. If the player sits back down first, it is hidden again.

Logs: `Hiding remote player actor ... reason=`, `Restoring remote player actor
... was vacated`, `Observed local furniture activation`, `Reactivated furniture
for the local player`.

### Ordering

- A playing attack finishes before furniture entry; furniture exit finishes
  before newer locomotion or attacks apply.
- Respawns and coordinate-space changes discard local furniture progress and
  reconstruct from the played checkpoint; historical transitions are not
  replayed.
- Shared-furniture resolution runs after coordinate-space and fast-travel
  retirement and before the actor update; local activations resolve before the
  local checkpoint is sampled.

## Mounts

A riding player appears on peers seated on a horse that follows its path at a
matching gait, or on a dragon that flies its path. The mount exists only while
the player rides and is removed with its rider in every case.

### Source

`MountSync::SampleLocalMount` reads the mount once per checkpoint (`IsOnMount`,
`GetMount`); position, heading, speed, `Direction`, swimming, and `pitch` are
then sampled from the mount. Speed is measured against the previous sample, so
mounting and dismounting cost one sample across the rider-to-mount offset. The
player counts as mounted for the whole interaction, including its animations.

A mount whose race has `kFlies` (`DragonRace`) is a flying mount: the
checkpoint sets `flyingMount` and carries its `ActorState` fly state
(`GetFlyState`), with speed measured along the flight
([Locomotion](locomotion.md#source-capture)). While it cruises (`cruising`,
`action`), the checkpoint also carries its wing stroke
(`ActorFlight::ReadWingState`): its graph's `Speed` against `TargetSpeedDamped`
times `TargetSpeedThresholdMin` and `Max`, as its own wing state modifier
compares ([Wing strokes](#wing-strokes)). `Publishing local flying mount
state` logs each fly state change, and `Local flying mount sample` logs the
dragon's graph flight inputs, wing stroke, controller pitch and roll,
`data.angle.x`, and measured heading and pitch rates every 2 s, for comparison
with the receiver's.

### Receiver

`MountSync::SyncMount` runs after the death and stranded-actor checks and before
every on-foot stage:

1. While the played checkpoint rides and the player is alive, a mount is spawned
   at the checkpoint through `ActorSpawner`, registered like a remote actor
   (survival, combat, magic, hostility, and collision isolation; activation
   blocked, so it cannot be ridden or stolen). `ActorMount::PickMountBase` gives
   a flying mount `EncDragon01FireNoScript` (`Skyrim.esm:0F8115`), a generic
   fire dragon without scripts (falling back to a horse), and every other spawn
   one of the five generic saddled vanilla horses at random
   (`EncHorseSaddledBrown`, `Black`, `BlackAndWhite`, `Grey`, `Palomino`;
   `Skyrim.esm:023AB2`, `068CFA`, `068D02`, `068D03`, `068D04`). The pick is
   local and per spawn; the allocation log names it (`base=`).
2. The mount takes each checkpoint as its target. Once ready,
   `ActorRuntime::UpdateMount` reasserts its policy, drives its [gait](#gait)
   or [flight](#dragon-flight), and translates it, including the
   [ground offset](locomotion.md#ground-offset). A checkpoint whose
   `flyingMount` differs from the spawned kind retires the pair as a dismount
   does.
3. Once both are ready, `ActorMount::Seat` seats the actor instantly, retrying
   every 0.5 s. From then on the mount carries it: it is never driven on foot
   again, so seating clears the on-foot graph tracking
   (`InvalidateAnimationDispatch`). `ActorRuntime::UpdateRider` keeps its
   policy, equipment and appearance are still reconciled, the weapon follows
   the checkpoint ([Weapon](#weapon)), and queued attacks are dropped. A
   dragon's rider gets no [gait](#gait) and sits in
   `Special_DragonMountSitIdle`.
   - The rider's `DragonMountInstant` idle sits under `DragonEvents`, which
     requires the dragon to carry `ActorTypeDragon` and Dragonborn's
     `DLC2DragonMountKeyword` (`Dragonborn.esm:01BD7D`), which the source's
     dragon gets from the `DLC2TameDragon` alias. `ActorMount` hooks
     `Character`'s `HasKeywordHelper` (vfunc 0x48) and reports the keyword only
     for the dragon being seated, on the main thread, during the start; adding
     it to the base would let the ride activation perk
     (`DLC2TameDragonActivatePerk`) match local dragons.
   - The dragon must also carry `kIsAMount` (`Actor::BOOL_FLAGS`), which Skyrim
     derives from the mount keywords only at initialization (ID 37653, SE
     36894). Without it Skyrim treats the dragon as a rider of its own
     interaction: the mount-chain walk (ID 41381) recurses until the stack
     overflows, and the graph managers link both ways.
     `ActorMount::AssertFlyingMount` sets it before the seat and on every mount
     update (`Marked remote flying mount as a mount`).
4. The seat is checked every 0.5 s; a rider found off its mount is logged and
   seated again. A seat waits while either actor is getting on or off
   (`kGettingOnOffMount`).
5. When the checkpoint no longer rides, actor and mount are retired together and
   the actor respawns on foot.
6. A mount whose handle is lost retires its actor; both are rebuilt while the
   player rides.

`ActorLifecycle` owns the mount beside the actor: every retirement and removal
removes the mount in the same pass, rider first, while still seated. A riding
player who dies is retired the same way and killed on foot where it stands.

Transitions are instant. Skyrim's paired `HorseEnter` and `HorseExit` clips
are not played, since they link both graphs, so every removal, halt, and retry
around them would need its own handling. Skyrim's dismount (ID 38705) is never
used: even in its quick mode it starts the paired `pa_HorseExit`
(`BSSynchronizedClipGenerator`), and removing both actors in the pass that
started it crashes in `NotifyAnimationGraph`.

### Weapon

A rider draws and sheathes with the checkpoint's `weaponDrawn`, through the
riding graph's mounted draw and sheathe.
`WeaponTransition::ApplyRiderWeaponState` sets the target and the registry's
authoritative weapon state, and while `WEAPON_STATE` differs calls
`DrawWeaponMagicHands` every update until the transition animates, as on foot.
A first draw waits for `IsReadyForWeaponDraw` (at most 1 s), and a drawn weapon
stays drawn while new hand objects are reconciled. Without this the rider would
keep the state it was seated with. `Remote rider weapon state changed` logs
each change.

### Gait

The mount's gait comes from the checkpoint's ground speed
([Locomotion](locomotion.md#source-capture)), kept as
`ActorMovement::groundSpeed`. Vanilla horse movement types are walk 125, run
450 (`Horse_Default_MT`), and sprint 600 (`Horse_Sprint_MT`).

- The mount stops (`moveStop`) once the speed stays below 20 for 0.2 s and
  starts at once. The gallop (`SprintStart`, `SprintStop`, `IsSprinting`)
  starts after the speed stays at 560 or more for 0.3 s and stops once below
  490 for 0.5 s; `SprintStop` precedes `moveStop`. Ground speed and the holds
  keep slope landings (726–808 in 3D at a ground speed of 460), single slow
  samples, and the relocation onto a fresh mount from flapping the gait.
- `Speed` and `SpeedSampled` take the ground speed once per new checkpoint;
  `Direction` stays zero.
- An edge records the request whether or not the graph accepts it. Every
  0.25 s the graph's `IsMoving` and gallop (`iState` tagged
  `iState_HorseSprint`) are compared with the request, and a mismatch sends
  `SprintStop`, `moveStart`, `moveStop`, or `SprintStart`, in that priority
  (`Corrected remote … graph gait`). A running remote mount always reads
  `IsMoving` false, for an unknown reason, so while moving this sends a
  rejected `moveStart` every 0.25 s. In a debug build the first two corrections
  and every 240th log `Remote … graph probe`.
- `AnimationFirewall` hooks `BSSpeedSamplerModifier::Update` and, for a graph
  whose actor the gait drives (`ActorRegistry::GetGaitSpeed`, mount and rider
  only), sets `goalSpeed` to the gait speed before `speedOut` is written
  (`Corrected remote gait speed sampler goal`). The vanilla horse behaviors
  sample `Speed` into `HorseSpeedSampled`, which the locomotion blends read,
  and an unknown writer zeroes a moving mount's graph `Speed` several times a
  second, which made the run blend shake. Rewriting `Speed` from the sync pass
  does not prevent it, since the zeros reach the sampler before the next pass.

`HorseBehavior.hkx` (vanilla packfile): `moveStart` enters `LocomotionState`
and `moveStop` leaves it; `ForwardLocomotionBlend_Horse` blends on
`HorseSpeedSampled` (slow walk 5, walk 125, slow trot 214, trot 304, run 450);
the sprint behavior is active while `IsSprinting` is set; a
`BSSpeedSamplerModifier` writes `HorseSpeedSampled` from `Speed`; there is no
`SpeedSampled`.

`ActorSpeedChannel` polls the actor's own movement, zero for a mount moved by
its transform, so a remote mount's channel reports the network speed; while no
one rides, the hook returns for local actors without the lock.

The rider's own `horsebehavior.hkx` (under `actors\character`) blends the same
way. Its `Speed` and `IsSprinting` follow the mount's and its `Speed` channel
reports it; its `Direction` channel reports zero; seating clears its jump
state. Its graph rejects `moveStart` and `moveStop`, so it gets no locomotion
events; it takes every native event, which reach it from its mount.

For a mount the firewall blocks Skyrim's gait events (`moveStart`, `moveStop`,
`SprintStart`, `SprintStop`, `moveForward`, `moveBackward`, `BrakeStart`,
`BrakeStop`, turns, canned turns) and passes its other native events. A seated
rider's registration is marked mounted, which admits every native event on it.
A 2 s gait sample per role logs `Speed`, `HorseSpeedSampled`, and the
controller's pitch and target.

### Dragon flight

A dragon is driven through its own vanilla `dragonbehavior.hkx`, so peers see
their own dragon animations. `ActorFlight::Apply` takes the played checkpoint's
fly state in place of the gait and, on each change, sends the matching vanilla
flight idle's event from `Skyrim.esm` through the network dispatch:

| New state | From | Event |
| --- | --- | --- |
| `takeOff` | any | `TakeOff` (`FlyStartTakeOff`) |
| `cruising`, `action` | `hovering` | `HoverStop` |
| `cruising`, `action` | `none`, `landing`, `perching`, first update | `FlyStartCruise` |
| `hovering` | `takeOff`, `cruising`, `action` | `HoverStart` |
| `hovering` | `none`, `landing`, `perching`, first update | `FlyStartHover` |
| `landing` | any | `FlyStopDefault` |
| `none`, `perching` | `cruising`, `hovering`, `action` | `FlyStopDefault` |

- A takeoff continues into flight and a landing onto the ground by
  themselves, so `takeOff` to `cruising` and `landing` to `none` send nothing.
  A flight state followed directly by `none` (a landing the playout skipped)
  still lands. The first update treats the previous state as `none`, so a
  dragon spawned mid-flight starts flying.
- A rejected event is resent every 0.25 s until the graph takes it (`Remote
  flying mount graph took a retried flight event`), its state already matches,
  or the fly state changes; a takeoff refuses `FlyStopDefault` until airborne,
  so a short hop's landing waits for its takeoff. `Remote flying mount state
  changed` logs each change with its event and result.
- Once a state has held 1.5 s (or 1.5 s after a retried event was taken) with
  no event pending, every 0.25 s the graph's `iState` is compared with it
  (`iState_DragonFlying`, `iState_DragonHovering`, anything else ground). A
  mismatch sends `FlyStartCruise` or `HoverStop` toward flight, `FlyStartHover`
  or `HoverStart` toward a hover, and `FlyStopDefault` toward the ground
  (`Corrected remote flying mount graph`). `takeOff` and `landing` are never
  corrected, since their clips last seconds. After a rejected correction the
  next check waits 2 s, since landing keeps `iState_DragonFlying` through the
  touchdown and rejects `FlyStopDefault`.

The graph reads its flight inputs from channels that poll the AI process, which
never flies a remote dragon, so `ActorFlight` publishes them every update
(`ActorRegistry::SetFlightChannels`) for the
[channel hooks](remote-actor.md#graph-channels):

- `TargetSpeed`: the checkpoint speed, divided by the speed ratio of the
  source's [wing stroke](#wing-strokes) while cruising.
- `TurnDelta`: the network heading rate in degrees per second, sign reversed,
  which banks the flight clips (a source dragon's `TurnDelta` mirrors its
  heading rate).
- `PitchDelta`: the network pitch rate smoothed over 0.2 s, in degrees per
  second, which bends the body.

Its `Speed` channel reports the checkpoint speed (`ActorRegistry::SetGaitSpeed`).
Flight events the local AI sends (`TakeOff`, `FlyStart`, `FlyStop`,
`HoverStart`, `HoverStop` prefixes, `HoverToCruise`) are blocked. The dragon is
network-placed like a horse, translating at 20 or more and otherwise held
standing. Its controller takes the checkpoint's pitch whatever its `kCanPitch`
flag ([Terrain pitch](#terrain-pitch)), and while flying `data.angle.x` takes
it too: Skyrim (ID 39548) builds a flying actor's graph `MovementDirection`
from `data.angle`, and the climb and dive clips blend by its height
(`MoveDirZ`, `FlightPitchBlend`). While it flies, `Remote flying mount sample`
logs the graph inputs, channel values, wing stroke and flags, controller pitch,
`data.angle.x`, `MoveDirZ`, and `FlightPitchBlend` every 2 s.

#### Wing strokes

The graph picks its stroke itself (`EEM_Wing_State`): `Flap` while `Speed` is
below `TargetSpeedDamped` × `TargetSpeedThresholdMin` (0.9), `Feather` above
`TargetSpeedThresholdMax` (1.1) × it, `Glide` between. `TargetSpeedDamped` is
`TargetSpeed` capped by a turn and pitch limit never below `MinSpeed` (2500).
A remote dragon fed the measured speed as both `Speed` and `TargetSpeed` would
read a ratio of one and only glide, so the source's stroke is sent
([Network](network.md#checkpoint-and-context)). The receiver keeps `Speed` at
the checkpoint speed and sets `TargetSpeed` so the ratio falls inside the
stroke's band, from its own graph's thresholds (read once per dragon): 0.9 ×
`TargetSpeedThresholdMin` to flap, the midpoint to glide,
`TargetSpeedThresholdMax` / 0.9 to feather; the margin covers the graph's own
damping. Outside cruising, without a stroke, or without the thresholds,
`TargetSpeed` is the checkpoint speed.

### Terrain pitch

A horse leans to its slope: the actor's 3D update (ID 37364) builds the body
rotation from the controller's `pitchAngle` and `rollAngle`, and a seated rider
tilts with it. The angles come from the ground only in the movement update,
which network placement skips, so a remote mount would stay level.

The source samples its carrier's pitch per checkpoint. On a peer the mount
update eases its controller's pitch toward it with a 0.1 s time constant,
hiding the 50 ms steps. Only a controller with `kCanPitch` (from the race flag
`kTiltFrontBack`, ID 78305) is pitched, as in Skyrim. Roll is not synchronized:
the vanilla horse does not roll.

Traced in the 1.7.104 binary: the orientation controller (`controller + 0x190`,
vfunc 8) moves each angle toward its target by `pitchMult` every movement
update. A horse's `bhkQuadOrientationController` (ID 80138) targets the slope
between ground heights below its front and rear feet (IDs 80142, 80143); the
biped one (ID 79327) follows the support normal, clamped to 47°; pathing
(ID 38308) eases toward the navmesh normal. All run only inside `Actor::Move`
(ID 37507), reached for a non-player actor only from the skipped movement
update. `Actor::SetPosition` (ID 37309) and `SetHeading` (ID 37230) leave the
angles alone, and `kNoSim` suppresses the move-finish sink (ID 37996) that
would overwrite them.

### Rider placement

Skyrim places a seated rider from its mount in a movement update that runs
before the sync pass ([Remote actor](remote-actor.md#graph-speed)), so the
rider would trail the mount by one step. After the mount is moved,
`ActorMount::CarryRider` moves a seated rider by the mount's step and turns its
offset and heading by the mount's turn; a placement later in the frame
overwrites it. Where Skyrim places the rider is not traced.

### Collision

The rider is left to Skyrim's movement update, which carries it, with its
character body out of every collision while seated; the mount is
network-placed with its body out of collision
([Remote actor](remote-actor.md#world-isolation)).

A seated rider's bone and weapon bodies are on `kBipedNoCC`, which the local
player's proxy controller collides with, and hang at the mount's flanks, so the
pass-through hook lets the player's movement update ignore every remote
character and skeleton body its phantom overlaps, for that update only.

They also overlap the mount's body, so while seated the rider's skeleton bodies
get the no-collision bit too, reasserted every 0.5 s (Skyrim rewrites the
filter around seating, and a rebuilt 3D brings new bodies) and cleared once
off the mount or dead. `Remote actor skeleton collision disabled`,
`reasserted`, and `restored` log each change with the body count.

## Death

When a player dies, peers see the actor die like a vanilla NPC: it collapses
into a ragdoll. Vanilla death ends in a load, whose world exit (`PeerLeave` with
`EXITED`) removes the body, so peers see it for the death camera's few
seconds.

### Source

`DeathSync::SampleLocalDeath` reads `ACTOR_LIFE_STATE` per checkpoint: `kDying`
and `kDead` publish `dead`; every other state, including `kBleedout` and
`kEssentialDown`, publishes alive. Checkpoints keep flowing until the load's
world exit. `dead` is checkpoint state, so a peer that joins, respawns the
actor, or re-enters the space during the death camera still sees a body.

### Receiver

`DeathSync::SyncDeath` runs after the coordinate-space checks and before every
stage that drives the actor:

1. A live actor whose played checkpoint is dead is killed once ready (3D,
   controller, graph); queued attacks are dropped.
2. A killed actor is left alone: only its handle is watched (a lost handle
   respawns it) and its appearance presentation kept.
3. A killed actor whose checkpoint is alive again (console `resurrect`, a
   reviving mod) is retired and respawned standing.

`ActorDeath::Kill` hands the actor to Skyrim:

- Its furniture is released first, including the hold published to the local
  activation hook.
- `RemoteCollision` releases placement (`owner=death`).
- `ActorRegistry::SetDead` marks the registration dead: the `KillImpl`,
  `KillDying`, `HandleHealthDamage`, and health `ModActorValue` guards let calls
  through, the update hook stops reasserting the survival policy, and the
  firewall admits every graph event.
- `kEssential` and `kProtected` are cleared.
- `Actor::KillImpl(nullptr, 0, false, true)` kills it with no attacker and no
  death event, so no local quest, script, crime, or statistic observes it, and
  ragdolls it immediately.

The body offers no interaction: no container, no activation prompt, no player
menu ([Remote actor](remote-actor.md#spawn)). The registration is per
reference, so a respawned actor starts protected again.

## Engine assumptions

Mounts, traced in the 1.7.104 binary:

1. A rider and mount are linked by an `ExtraInteraction` on the rider holding a
   `MountInteraction`; `Actor::GetMount` and `IsOnMount` read it.
2. Skyrim's mount entry (ID 38704) builds a `MountInteraction` only on its
   animated branch; its quick branch (`PutActorOnMountQuick`) restarts an
   existing one. `ActorMount::Seat` allocates 0x38 bytes with `RE::malloc`,
   constructs the interaction with ID 40345 (rider, mount), and starts it quick
   with ID 41530. It is reference counted at offset 8 and freed by the engine's
   deleting destructor. On SE: mount entry 37759, constructor 39266 (same
   code), start 40525.
3. Removing a seated pair together is safe: seated riding has no paired clip,
   and Skyrim itself drops a rider whose mount is gone (dismount mode 2 from
   IDs 36564, 37347, 40438).
4. A seated rider's controller stays in the world. After moving a ridden mount,
   its movement update (ID 37350) tries the rider's controller at
   `actorMountPosition` (ID 78260) to set `kMountPointClear`. A proxy
   controller's rigid body (vfunc 0x10, offset 0x5A0) is what other characters
   bump; its world collision is a separate phantom. The contact pass
   (ID 79130) records one bumped body per controller (ID 78303), so the
   pass-through hook frees a mover from only one of two overlapping bodies per
   update.

Death:

5. `KillImpl` with `a_sendEvent = false` still performs the full death: life
   state, ragdoll, stopped AI. Clearing `kEssential` is enough to kill an actor
   from a remote player's base.
6. A dead temporary reference keeps a valid handle while its cell stays loaded;
   if Skyrim cleans it up, the body is respawned and killed again.
7. The player's life state passes `kDying`/`kDead` on a vanilla death before the
   load opens.

Furniture, traced in the 1.7.104 binary:

8. ID 39865 is `bool(AIProcess*, Actor*)`. From IsSitting or IsSleeping it
   processes a `TESActionData` (kTry, `ActionActivate`, target the occupied
   furniture) and, if accepted, sets WantToStand or WantToWake through
   ID 39912, keeping the furniture handle and marker; from those states it does
   nothing.
9. `ChairFurnitureExitHandler` and `BedFurnitureExitHandler` return an actor to
   Normal and clear its occupancy (ID 39912 with no furniture) at the end of
   the exit clip.
10. With a weapon drawn, ID 39923 calls the actor's sheathe (vfunc 0xA6) and
    returns true without an entry point, so ID 37330 reports success with
    marker -1. ID 39861 runs its entry step only from Normal, setting WantToSit
    or WantToSleep (ID 39912) and reserving the process's marker (ID 20159)
    without checking one was chosen. That the reset to Normal fixes the offset
    seat is not verified.
11. The SE 1.5.97 counterparts, matched by instruction sequence and callees:
    19767 (20162), 36340 (37330), 38909 (39955), 38882 (39923), 36334 (37324),
    36342 (37332, calling 38831 for 39861), 38835 (39865).
12. `AIProcess::ClearFurniture` (ID 39798, SE 38773) given the actor releases
    the process's marker on the occupied reference for it (ID 20159, SE 19764)
    before clearing the occupancy; without it the reservation stays. Every
    receiver call passes the remote actor.

Dragon flight:

13. `dragonbehavior.hkx` takes `TakeOff`, `FlyStartCruise`, `FlyStartHover`,
    `HoverStart`, `HoverStop`, and `FlyStopDefault`, the `ENAM` events of the
    `Skyrim.esm` idles `FlyStartTakeOff` (`03B553`), `FlyStartCruise`
    (`03BDA6`), `FlyStartHover` (`03BDA7`), `HoverStart` (`017183`),
    `HoverStop` (`03B559`), and `FlyStopDefault` (`040B4A`). Which state each
    transition leaves from is inferred from the idle names.
14. Its modifiers derive the wing state as in [Wing strokes](#wing-strokes),
    bank from `TurnDelta`, and bend from `PitchDelta`, both scaled by
    `TimeStep` and `TurnDeltaScale`, so both are rates.
    `ActorTargetSpeedChannel` (ID 38331) reads ID 37959 and
    `ActorPitchDeltaChannel` (ID 38328) reads ID 37963 × 57.2958, both from the
    AI process; the `TurnDelta` channel copies its value unscaled. `PitchDelta`
    does not follow the source's 2 s pitch rate, so it may be an angle to a
    target. ID 39548, called by the actor update (ID 37350) for a high process
    with a fly state other than `none` and `perching`, stores the forward
    direction of `data.angle` composed with the middle-high process's rotation
    (`+0xB0`, ID 91804) as `MovementDirection`; that a remote dragon reaches it
    is assumed.
15. The graph's flight notifications (`FlightTakeOff`, `FlightCruising`,
    `FlightHovering`, `FlightLanding`, `FlightLanded`) reach Skyrim's
    `Flight*Handler`s, which set the fly state; a network-placed dragon needs
    nothing else to stay airborne, since the skipped movement update never
    applies gravity.
16. The quick start (ID 41530) runs the interaction's start (vfunc 0x70,
    ID 40351), which processes `ActionMount` at once and fails (result 2) when
    the idle tree yields no idle. Under `DragonEvents` (`02017715`) the quick
    branch picks `DragonMountInstant` (`02017716`, event
    `DragonMountEnterInstant`). The `HasKeyword` condition (ID 21644) calls
    vfunc 0x48, which `Character` takes from `Actor` (ID 37183, SE 36204).

Mount gait:

17. `BSSpeedSamplerModifier` overrides only vfuncs 0, 5 (`Update`, ID 63306),
    and 0xC; `Update` reads `state`, `direction`, and `goalSpeed` at 0x50, 0x54,
    and 0x58 and writes `speedOut` at 0x5C. A `BShkbAnimationGraph` builds its
    `hkbContext` from its `characterInstance` at 0xC0 (IDs 63562, 63563) and
    keeps its `Actor` at 0x210. The same layout is assumed on SE (vtable from
    CommonLib ID 282071).

## Acceptance criteria

Furniture:

1. Entering a resolvable reference makes the actor use the same reference with
   its native entry idle, starting close to the source's entry; leaving plays
   the native exit and returns to the latest locomotion.
2. Dropped snapshots and late joins converge to the newest reference or none; a
   player already in furniture appears in it without an entry idle.
3. Furniture use pauses translation and cannot overlap an attack, jump, or
   weapon transition. Unresolved, non-furniture, and cross-space references,
   missing markers, and stuck transitions are bounded, logged, and retried.
4. The local player can use furniture a remote actor holds without an "in use"
   message; the actor is hidden while its source stays or the player keeps the
   last free marker, and returns seated once vacated. Two actors never stack on
   one marker.
5. A remote actor still uses furniture after being hit.
6. Peers whose load orders differ but that load the furniture's plugin see each
   other use it, including furniture a light plugin adds.

Mounts:

7. A mounting player appears seated on every peer, also for late joiners. The
   horse follows its path and walks, runs, and gallops with its speed, with few
   `Corrected remote mount graph gait` lines, and the rider's
   `horseSpeedSampled` is near the mount's.
8. A dismount removes the horse and leaves the actor standing where the player
   got off. Death, leaving the world, disconnecting, or timing out removes both
   without a crash.
9. The horse cannot be ridden, looted, or harmed locally, and no local actor
   treats it as hostile. The local player walks through a rider and its horse,
   on foot or riding.
10. A horse on a slope leans with it, and its rider with it.
11. A rider draws and sheathes when the source does on horseback, whatever it
    was seated with, and a hand item changed on horseback appears without a 2 s
    delay.
12. A player riding a dragon appears seated on a dragon on every peer, also when
    joining mid-flight. The dragon takes off, flies, hovers, and lands when the
    source's does, with few corrections and no rejected flight events, banks
    into turns, and flaps, glides, or feathers with its speed. Dismounting
    removes it like a horse.

Death:

13. A player killed by an enemy collapses into a ragdoll on every peer, and the
    body disappears when its game loads; a peer entering the cell during the
    death camera sees it collapse where the player died.
14. The body cannot be looted, shows no activation prompt, and opens no player
    menu. The local player gains no kill, bounty, or quest progress.
15. A player revived with `resurrect` stands up as a fresh actor; one in
    bleedout (essential) stays standing.

## Known limitations

- The gallop is inferred from speed. A galloping source's measured speed swings
  between about 345 and 615; the 0.5 s stop hold is meant to bridge the dips.
  Publishing the source mount's sprint state would remove the guess.
- Furniture: a native exit plays its full clip while the source may already be
  walking away (up to 8.3 s for a bed); the actor then catches up along the
  network path, or snaps if the source stands still by then.
- Furniture: crafting, recipes, skill progression, sleep and wait UI, ownership
  and crime, arbitrary activators, doors, containers, scripted furniture, and
  paired idles are not synchronized; there is no global reservation across
  clients; a peer without the furniture's plugin shows the actor standing.
- Mounts: the source's mount appearance (a random vanilla horse stands in, and
  one vanilla dragon for every dragon), mount and dismount animations, combat
  on horseback (a drawn weapon is shown), horse jumps, a mount left standing
  after a dismount, and the mount's approach are not shown.
- Dragons: breath and other shouts, the rider's spells and shouts in flight,
  flight kills, and perching (shown as a landing) are not shown. Banking comes
  from the receiver's graph through `TurnDelta`, so it may differ from the
  source's. The wing stroke follows the source's at checkpoint rate. A modded
  mount that is neither a horse nor a dragon shows a horse.
- Death: the killer, death animations including a kill move's paired death
  (the actor always goes straight to ragdoll), where the body comes to rest,
  and bleedout are not shown.
