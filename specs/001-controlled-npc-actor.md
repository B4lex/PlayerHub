# Controlled NPC Actor POC

## Status

Superseded as a runtime demo by `002-network-synchronization.md`. The
controlled-actor implementation remains the presentation layer, but its target
now comes from remote player snapshots instead of the local player-following
test.

Remote actors currently use the local `PlayerCharacter`'s actor base. This is
only a visual placeholder until the network protocol carries each remote
player's appearance and equipment.

## Goals

- Prove that an actor can be spawned and moved as a network-style actor.
- Keep local AI from navigating or choosing a conflicting movement destination.
- Provide basic idle, walk, and run presentation while direct transform movement remains authoritative.
- Keep all Skyrim object access on the game thread.

## Non-goals

- Navmesh pathfinding, collision avoidance, or ordinary follower AI.
- Network transport, snapshot buffering, interpolation delay, or ownership.
- Full animation replication for combat, sneaking, mounts, weapons, facial expressions, or modded behavior graphs.
- Cross-cell/worldspace transitions.

## Runtime architecture

```text
Skyrim main-loop call hook
    -> FrameUpdate::OnFrame on one bound owner thread
        -> create one ControlledNPC when absent
        -> set targetPosition to the player's current position
        -> NPCSpawner::UpdateControlledNPC
            -> calculate follow movement and direct transform step
            -> apply graph-variable/event changes before the transform step
```

`FrameUpdate::OnFrame` owns the single prototype actor. It creates the actor after the player has a valid parent cell, retries a failed spawn after one second, and discards the stored controller when its actor handle becomes invalid.

## ControlledNPC state

`NPCSpawner::ControlledNPC` contains:

- `RE::ActorHandle handle`: stable, non-owning reference to the spawned actor.
- `RE::NiPoint3 targetPosition`: current target in world coordinates.
- `float translationSpeed`: positional interpolation speed in game units per
  second.
- `float animationSpeed`: graph speed from the same network sample as the
  current position and movement state.
- `float stoppingDistance`: radius at which movement stops.
- `MovementState targetMovementState`: replicated source-player locomotion.
- `shared_ptr<AnimationState> animationState`: latest requested state and speed,
  last processed state, and request/processing revisions.
- `IAnimationGraphManagerHolder* animationGraphHolder`: identity used by the
  actor-only graph-event filter; it is never dereferenced after registration.
- `bool ready`: actor 3D, character controller, and animation graph manager have
  initialized.

The public API is intentionally small:

```cpp
void InstallRemoteReplicaHooks();

std::optional<ControlledNPC> SpawnControlledNPC(
    RE::TESNPC* npcBase,
    const RE::NiPoint3& targetPosition,
    float targetHeading,
    Multiplayer::MovementState movementState,
    float speed = 250.0F,
    float stoppingDistance = 1.0F);

void SetTargetTransform(
    ControlledNPC&,
    const RE::NiPoint3&,
    float targetHeading,
    float translationSpeed);
void SetAnimationState(ControlledNPC&, Multiplayer::MovementState);
bool SendNetworkAnimationEvent(
    ControlledNPC&,
    const RE::BSFixedString& eventName);
bool IsAnimationStateSettled(const ControlledNPC&);
void RemoveControlledNPC(ControlledNPC&);
bool UpdateControlledNPC(
    ControlledNPC&,
    float deltaTime,
    bool allowTranslation = true);
```

Future network code must not call `RE::Actor` APIs from its receiver thread. It should enqueue a target/snapshot for the game-thread update path.

## Spawn and initialization behavior

1. `CreateReferenceAtLocation` creates the persistent actor directly at the
   first remote snapshot's position and heading. Exterior spawning waits until
   the cell containing that position is loaded; it never creates the actor at
   the local player as an intermediate location. Because the special
   `PlayerBase` record (`00000007`) does not complete this generic placement
   path, the reference is constructed with a regular NPC base and immediately
   switched to `PlayerBase` before its 3D initializes.
2. Activation is blocked so the player cannot interact with the proxy.
3. Actor processing remains enabled throughout the actor lifetime. Disabling
   it clears `kProcessMe`, zeroes controller velocity, and can prevent the 3D,
   controller, or graph from initializing and updating.
4. The current proof of concept uses the local `PlayerCharacter` actor base for
   every remote actor. Appearance replication is separate from
   remote-animation ownership; a future protocol must select the remote
   player's actual base appearance and equipment.
5. Attack/cast decisions are disabled on each spawned reference. Immediately
   before Skyrim's normal actor update, the live AI process is marked as
   ignoring combat and any combat controller is marked inactive. The original
   update still runs, preserving graph and controller processing.
6. Reference, 3D, and character-controller collision remain enabled, so each
   actor still stands on the world. A hook on the actor movement update call
   (ID 37350 + 0xFB, as in powerofthree's Tweaks' TCL fix) sets the Havok
   no-collision filter bit on the character body a controller bumped into for
   the duration of that update, when either the mover or the bumped body's
   reference is a remote actor, so remote actors never body-block each other
   or the local player. The `kNoCharacterCollisions` controller flag does not
   reach that body. The reference must not carry the `kCollisionsDisabled`
   flag that `TCL` toggles: for any actor, it switches the movement update
   (ID 37356) to TCL's noclip integration, which skips character-controller
   simulation. Skyrim still enters swimming from the actor's submerged depth,
   but it leaves swimming only once the controller reports support or its
   in-air state (ID 37348), so a noclip actor keeps swimming on land. Local
   combat reactions are suppressed at their decision/event paths.
   Local actors never become hostile to a actor; see `008-remote-actor-combat-isolation.md`.
7. `InitiateDoNothingPackage()` removes autonomous package travel once the 3D
   and controller are ready.
8. At spawn time, the actor is registered with a hook on
   `Character`'s `IAnimationGraphManagerHolder::NotifyAnimationGraph` entry.
   The firewall activates after the graph accepts its initial network state, so
   graph construction is not filtered. Replicated locomotion is dispatched
   directly to the saved graph function, bypassing the public local-event
   filter; events injected by local gameplay or mods are rejected and logged
   per event name. The firewall opens again while a replacement graph is
   initializing, then reactivates after the network state is restored.
9. A `TESHitEvent` for a registered actor queues graph recovery instead of
   mutating the graph from inside Skyrim's hit callback, which may overlap
   animation work. On the next game-thread actor update, the firewall opens
   and Skyrim's native `RevertAnimationGraphManager` resets the complete graph,
   including variables and partially active transitions. `DoReset3D(false)` is
   used only when the native graph revert fails or does not become ready within
   120 update passes. Translation remains paused
   until the graph and controller are ready for two consecutive passes; the
   latest network locomotion state and speed are then replayed from an
   invalidated synchronization revision. Stagger/recoil polling also catches
   interruptions without a hit event.
10. The update loop never calls `MoveTo(player)`: an exterior actor can
   legitimately occupy a different parent cell from the local player.

## Follow and transform movement

The demo refreshes the target to the player position every frame and uses a 512-unit stopping radius.

- If the actor is within `stoppingDistance + 1.0`, it remains idle.
- Otherwise it moves along the normalized target offset.
- `deltaTime` is clamped to 0.1 seconds to avoid a large catch-up move after a stall.
- The movement step is capped at `distance - stoppingDistance`, so it does not overshoot the follow radius.
- `SetPosition(nextPosition, true)` updates both the reference transform and the actor character controller.
- The actor heading comes from the remote body angle, not the movement vector; Skyrim heading zero faces positive Y. Backward and sideways travel is expressed through the graph `Direction` variable.

The demo currently spawns the actor beside the player. It begins moving only after the player is farther than 512 units away.

## Locomotion presentation

Transform motion remains authoritative. Animation is presentation only. The
source client's explicit movement state is authoritative; the actor does not
validate a transition against its previous local state.

- Graph mutation and direct translation run in the same main-loop pass, with
  graph mutation first. Transform translation pauses until the matching
  animation request revision has been processed.
- A state is latched after its graph variables and required events are accepted.
  Because Skyrim can also return false while an edge is already active or
  transitioning, event retries are capped at four; variable failure remains
  pending until the graph is writable.
- `Speed`, `SpeedSampled`, `Direction`, `TurnDelta`, and `IsSprinting` are
  refreshed at the local game-frame rate. Speed comes from the same network
  sample as position and movement state.
- Locomotion state coalesces with its transform sample. Compound changes are
  staged one graph update at a time, such as `idle -> running -> sprinting`, so
  dependent events are not sent together, but skipped low-level locomotion
  states are not replayed as historical actions.
- Actor translation uses the network snapshot's numeric speed and never
  infers an animation from either speed or target distance.

This is a vanilla-humanoid proof of concept. Foot sliding, unusual transitions, and incompatibility with custom behavior graphs are expected until network snapshots include complete animation state.

## Crash-safety revision

The first animation implementation directly modified `Actor::actorState1` movement bits during the player animation hook. A crash log then showed the dynamically created Nazeem actor failing later in Skyrim's FaceGen update path. The failure did not contain a direct `skyrim-mp.dll` frame, indicating delayed invalid engine state rather than a direct fault in the hook.

The implementation was revised as follows:

- Do not write `actorState1` or other engine-owned movement-state fields.
- Do not call animation graph mutation APIs from
  `PlayerCharacter::UpdateAnimation`; that hook can execute on animation workers.
- Run the complete synchronization pass from Skyrim's main-loop call site and
  bind its first observed Windows thread ID as the sole engine-mutation owner.
- Refuse all public actor operations that access Skyrim objects from another
  thread, and stop updating if the main-loop hook ever migrates threads.
- In that pass, revalidate the actor handle, loaded 3D, and
  `BSAnimationGraphManager` before setting variables or sending graph events.

The earlier queued-graph revision compiled and linked successfully. The current
game-thread/coherence and remote-ownership changes still require a user-run
Windows build and in-game stability test. Blocked local graph events are logged
with the actor handle and event name so any remaining interference can be
traced.

## Known limitations and next steps

1. Replace the local player-base placeholder with a network-selected character
   appearance and equipment description.
2. Move from a single `ControlledNPC` to an ID-keyed registry owned by the game thread.
3. Queue network snapshots rather than directly assigning target positions from a receiver thread.
4. Render behind estimated server time and interpolate between ordered position/rotation snapshots.
5. Extend replicated animation beyond the current locomotion/sneak/sprint states.
6. Handle actor unloads, save/load, cells, interiors, worldspaces, and respawn conditions explicitly.
7. Verify graph ticking, collision, and combat suppression across exterior
   cell/3D reloads while normal actor processing remains enabled.

## Verification

The last verified baseline was compiled with the project MSVC/CommonLib
configuration and linked into:

`build/windows/x64/default/skyrim-mp.dll`

Run Skyrim in a fresh process; SKSE plugins are not hot-reloaded. Prefer testing from a save created before an older prototype actor was spawned.
