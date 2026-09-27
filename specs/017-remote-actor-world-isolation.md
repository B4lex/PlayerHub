# Remote Actor World Isolation

## Status

Implemented. Placement was verified in game on 1.7.104 with a replayed player
walking, idling, mounting, riding, and dismounting; the other acceptance
criteria are not yet checked. The server side is covered by unit tests. The
engine behavior below was traced in the 1.7.104 binary.

## Goal

The local world neither moves a remote actor nor is moved by it. The network
transform is the only thing that places a remote actor: local geometry,
characters, and physics objects do not push it back, and it does not block the
local player or push local clutter, ragdolls, or characters.

Previously each actor update integrated the remote actor's character
controller from wherever the network had last put it. The controller added the
animation's root motion, pushed out of anything it overlapped, and pushed local
bodies. The next main-loop pass moved the actor back toward the network target
from that displaced position, so an actor touching the local world shook.

## Non-goals

- Skeleton (ragdoll bone) bodies. Local projectiles, melee, traps that test
  them, and the crosshair still find the remote actor. See
  [Follow-ups](#follow-ups). The local player's own movement ignores them, and
  a seated rider's are isolated while it rides
  (`specs/016-mount-riding-sync.md`).
- A remote actor while native furniture, a mount, or its own death places it.
  Skyrim keeps moving it then, as before.

## Protocol

`player:update` and `player:state` carry `swimming`, a boolean that is true
while the actor the checkpoint places swims: the player, or the mount it rides.
The server validates it as a boolean and defaults it to `false` when missing,
so recordings made before this change replay as not swimming. Every relayed
`player:state` includes it, and the client requires it.
`MovementSync::SampleLocalMovement` reads it from the carrier's `ActorState`.

## Placement ownership

`ActorRuntime` decides on every update who places the actor, and
`RemoteCollision` publishes it through `ActorRegistry` (`networkPlaced`):

- An on-foot actor is network-placed once it is ready, except while
  `ActorFurniture::AdvanceFurnitureInteraction` reports that native furniture
  owns it.
- A mount is network-placed while it is driven or placed, and released to
  Skyrim while a mount or dismount clip holds it.
- A rider is always left to Skyrim, which carries it with its mount.
- A dead actor is left to Skyrim's death and ragdoll: `ActorDeath::Kill`
  releases it (`owner=death`) before the kill, since a dead actor is never
  updated again, and the registry's `dead` also overrides `networkPlaced`.

`Remote actor placement owner changed` logs each change, with the new owner.

### Skipped movement update

The existing hook on the movement update call (ID 37350 + 0xFB, the only call
of ID 37356) returns without calling it for a network-placed actor. For every
other actor it keeps the pass-through behavior: a remote actor that Skyrim moves
passes through the character body it bumped for that update.

ID 37356, per actor:

1. Reads the animation's root motion and the movement controller's output,
   passes them through `ModifyMovementData` (Actor vfunc 0x11A), and stores the
   AI process movement speed (`high + 0x2A8`, ID 39873).
2. Applies the heading change (`SetHeading`, ID 37230).
3. Calls `Actor::Move(delta, translation)` (vfunc 0xC8, ID 37507), which
   integrates the character controller. With the TCL flag (form flag bit 4) it
   instead adds the translation directly through `SetPosition`.
4. Unless the actor flies, compares the requested motion with the position
   change around `Move`, and feeds the result into the graph (vfuncs
   0x11D/0x11E).

Skipping only `Move` would leave step 4 measuring zero motion. Skipping the whole
update removes the second writer of the actor's position and heading.

### Frame order

The main-loop hook wraps the call at ID 36564 + 0x6E (ID 40438). That call
reaches the per-frame actor movement job list (ID 41306 with job callback ID
41444, which calls ID 37350), so Skyrim's movement update runs before
`SyncCoordinator::OnFrame`, and the network transform is the last write before
render. The movement update runs on job threads, so `IsNetworkPlaced` takes the
registry lock, with an atomic count that skips it when no remote actor is
network-placed.

## Graph speed

The graph's Speed channel (ID 38330 → ID 42767) reads `Actor::GetMovementSpeed`
(ID 37957 → ID 39872), the process value step 1 above stores. A network-placed
actor no longer updates it. `ActorAnimation` publishes the graph Speed it
writes to the registry when it changes, and the Speed channel hook reports it
for a living network-placed actor. A gait-driven mount or rider keeps reporting
its gait speed. The Direction and TurnDelta channels were already overridden.

## Swimming

Swimming is decided in the actor update (`Character::Update`, vfunc 0xAD, ID
37348) from the submerged fraction `(water height − reference Z) / height` (ID
37448), which follows the network position:

- At 0.7 or deeper it starts swimming (ID 37946) and requests controller state
  5 (swimming).
- Below 0.7, while swimming, it stops swimming only when the controller's
  `kSupport` flag (`controller + 0x218`, bit 8) is set or the controller's
  current state (`context + 0x20`) is in-air. Otherwise, below 0.01, it requests
  the in-air state.

These are requests: `Character::Update` writes the controller's `wantState`
(`controller + 0x21C`), and only the controller integration inside `Move`
applies it. With `Move` skipped, neither the flag nor the current state ever
changes, and an actor that started swimming keeps swimming on land.

`RemoteCollision::PlaceByNetwork` therefore sets `kSupport` whenever the
source's checkpoint is not swimming, and clears it while it is. The engine's own
exit path then ends swimming once the actor is shallower than 0.7 and its
source has stopped swimming, as it does for a grounded character. An actor
deeper than 0.7 in local water keeps swimming even if its source does not.
`Remote actor controller support changed` logs each change.

## Character body

Skyrim gives the player a proxy controller and every other actor a rigid-body
controller (ID 37171 constructs `bhkCharProxyController` only for the player, or
when a global Havok setting or a race flag asks for it). A rigid-body
controller's character body (`GetRigidBody`, ID 79189, the body at `+0x350`) is
also its world collision: the physics step simulates it whether or not the
movement update runs, so it collides with, pushes, and is pushed by local
bodies, and triggers local volumes.

`RemoteCollision` sets Havok's no-collision filter bit on that body while the
network places the actor, or while its mount carries it, and clears it whenever
Skyrim moves the actor, so a seated, dismounting, or dying actor still stands
on the world. Each pass reasserts it, since a rebuilt controller collides again.
`Remote actor character body collision disabled` and `restored` log each
change, with the owner and the controller type.

- The filter core (ID 78553), which the pair, shape, and ray filters all call,
  rejects any pair where either side has bit 14 set, unless the first side's
  layer is 0x2C. The body therefore collides with nothing, and ray queries skip
  it.
- The bit is set without Havok's `updateCollisionFilterOnEntity`, as the
  pass-through hook already does, so it takes effect for new contacts and
  queries.

### Body write-back

Every physics step, the rigid-body controller's post-simulation callback (ID
79185, on its second vtable) reads the controller position (`GetPositionImpl`
with the center offset, ID 79194), which applies the body's rotation to the
offset between its center and the actor's feet, and sends it through the
controller's move-finish event source (`controller + 0x10`, ID 38322) to the
actor's sink (`actor + 0xD8`), which places the actor there. `Actor::Move` sends
the same event itself. The callback skips it only while the controller's
`kNoSim` flag (bit 17 of `controller + 0x218`) is set.

Only the skipped movement update keeps that body upright and at the actor. A
body left tumbling after a dismount, with nothing to collide with, therefore
carried the actor around a circle of about 10 units in a vertical plane every
physics step, and each pass pulled it back.

`RemoteCollision` sets `kNoSim` while the network places the actor, and clears
it when it hands the actor to Skyrim, including at death. Skyrim's own
temporary uses (ID 37314, ID 37652) save and restore the flag; the AI procedure
that clears it (ID 32797) is followed by the next pass reasserting it.
`Remote actor character body places the actor` and `no longer places the actor`
log each change.

## Engine assumptions

Checked in game: with its movement update skipped and `kNoSim` set, a
network-placed actor, on foot or a mount, stays exactly where the network
transform puts it, and its 3D follows `SetPosition(..., true)`. A temporary
check of each actor's position between passes found no other writer across
replayed mounts and dismounts.

Not yet checked in game:

1. A locomotion graph driven by the published Speed plays at the same speed as
   before.
2. Setting `kSupport` outside `Move` is not overwritten by any other path while
   `Move` is skipped.
3. Pressure plates and other trigger volumes detect actors through the character
   body, so remote actors no longer trigger them.

## Acceptance criteria

From `latest.log` and in game:

1. A remote actor walking through local NPCs, a doorway, or clutter, or idling
   inside a local object, moves smoothly on its network path without shaking,
   including after a dismount.
2. The local player walks through a standing and a moving remote actor, on foot
   and riding. Local clutter and bodies are not pushed by remote actors.
3. A remote actor that swims logs `supported=false`, swims, and leaves swimming
   when its player wades out (`supported=true`), including up a shallow shore.
4. Jumping, landing, sprinting, sneaking, and attacks look as before. The
   locomotion speed matches the player's.
5. A remote actor sits in, uses, and leaves furniture (`owner=furniture`, then
   `owner=network`).
6. A remote player mounts, rides, and dismounts (`owner=mount clip`,
   `owner=mount`, `owner=network`), and its horse follows the network path.
7. A dying remote actor still ragdolls onto the local ground.
8. A remote actor walking onto a pressure plate does not trigger the trap.

## Follow-ups

Remote actors should also be neither hittable nor targetable: they still block
local arrows and can be struck. Those queries reach the skeleton's bone
bodies, not the character body. Isolating them means setting the same bit on
every bone body of the actor's 3D, reapplying it when the 3D is rebuilt, and
restoring it on death so the ragdoll still lands on the ground.
