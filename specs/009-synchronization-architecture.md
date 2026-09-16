# Synchronization Architecture Restructure

## Status and objective

Implemented (pending Windows build and in-game verification; see
Implementation notes). This document describes a code organization change, not a
protocol or gameplay change. Preserve the current visible behavior, wire format,
timing values, engine hooks, fallback rules, and logging needed to compare runs.
The existing implementation in `src/` is the authority wherever an older spec
describes a different behavior.

The goal is to give each synchronization concern an explicit home and a frame
entry point, while keeping one coordinator responsible for ordering. The frame
hook and NPC creation/removal belong outside the synchronization modules.
The refactor also transitions project-owned `Puppet` and `NPCSpawner` names to
`Actor` names for the remote player representation.

## Current seams in the code

| Current code | Responsibilities to retain | Proposed owner |
| --- | --- | --- |
| `FrameUpdate.cpp` `MainLoopUpdateHook`, `Install` | Main-loop call detour, original call first, delta validation, game-thread binding, trampoline allocation | `runtime/MainLoopHook` and `runtime/PluginStartup` |
| `FrameUpdate.cpp` `OnFrame`, `ApplyNetworkChanges`, `RemotePuppet` | Drain once, disconnect/sequence handling, shared peer lifetime, fixed execution order | `sync/SyncCoordinator`, `sync/PeerState` |
| `FrameUpdate.cpp` `GetAnimationState`, `GetNextAnimationStep`, `AdvanceAnimationState`, graph epoch | Local locomotion/jump/stance capture and staged graph targets | `sync/animation/AnimationSync` |
| `FrameUpdate.cpp` `PublishLocalTransform`, coordinate-space checks, `NPCSpawner.cpp` transform setters/interpolation | 20 Hz checkpoint cadence, position, heading, speed/direction, compatible space | `sync/movement/MovementSync` plus `actor/ActorMovement` |
| `FrameUpdate.cpp` local attack observation, queued actions, `AdvanceAnimationAction` | Attack edge capture, graph-epoch/action ordering, bounded queue and dispatch | `sync/combat/CombatSync` |
| `FrameUpdate.cpp` furniture capture and target staging, `NPCSpawner.cpp` native furniture lifecycle | Occupancy checkpoint, entry/use/exit and graph ownership | `sync/furniture/FurnitureSync` plus `actor/ActorFurniture` |
| `PlayerAppearance.*` | Local revisions, received appearance, base construction, tint, respawn request | `sync/appearance/AppearanceSync` (may initially wrap existing files) |
| `PlayerEquipment.*` | Local revisions/events, vanilla resolution, remote actor inventory reconciliation, hand readiness | `sync/equipment/EquipmentSync` (may initially wrap existing files) |
| `NPCSpawner.cpp` spawn/init/remove and `UpdateControlledNPC` | Actor creation and retirement; per-actor engine application | `actor/ActorSpawner`, `actor/ActorRuntime` |
| `NPCSpawner.cpp` graph firewall, event observations, damage/magic/combat detours and survival policy | Hook installation, registration, worker-thread-safe identity, isolation | `actor/ActorRegistry`, `actor/AnimationFirewall`, `actor/CombatIsolation`, `actor/SurvivalPolicy` |
| `SocketIOClient.*`, state/form headers | Transport queue, validation, serialization; shared protocol types | Client `network/` and `protocol/` |
| `server/src/server/*` | Server validation, relay, and recordings | Keep in `server/`; verify unchanged behavior |

`NPCSpawner.cpp` is currently both an actor factory and a large behavior
controller. Moving its functions by responsibility matters as much as splitting
`FrameUpdate.cpp`; otherwise the frame file shrinks while the same pile remains.
Do not split the graph state machine into arbitrary one-function files: its
landing, draw/sheathe, swim, recovery, and native attack transitions share one
graph lifecycle and should remain one coherent animation controller, with
private helpers if its implementation needs several translation units.

## Proposed source tree

```text
src/
  main.cpp
  runtime/
    MainLoopHook.h/.cpp          # one Skyrim main-loop detour only
    PluginStartup.h/.cpp         # trampoline, hook and data-loaded registration
    GameThread.h/.cpp            # bound thread check for engine mutations
  protocol/
    MovementState.h             # MovementState and AnimationState value types
    PlayerState.h               # PlayerTransform and action DTOs
    AppearanceState.h
    EquipmentState.h
    FormReference.h
  network/
    SocketIOClient.h/.cpp       # transport, parsing, bounded pending queues
  sync/
    SyncCoordinator.h/.cpp      # one frame entry, fixed order, one drain
    PeerState.h                 # peer lifetime and per-module state composition
    Snapshot.h                  # per-pass local sample/checkpoint builder
    movement/MovementSync.h/.cpp
    animation/AnimationSync.h/.cpp
    furniture/FurnitureSync.h/.cpp
    combat/CombatSync.h/.cpp    # semantic attack presentation only
    appearance/AppearanceSync.h/.cpp
    equipment/EquipmentSync.h/.cpp
  actor/
    ActorSpawner.h/.cpp         # create and retire remote actor reference
    ActorRuntime.h/.cpp         # readiness and fixed engine application order
    ActorMovement.h/.cpp        # direct transform and heading application
    ActorAnimation.h/.cpp       # graph state machine, recovery, event dispatch
    ActorFurniture.h/.cpp       # Skyrim native sit/sleep lifecycle
    ActorRegistry.h/.cpp        # registered remote actor identity and hook observations
    AnimationFirewall.h/.cpp
    CombatIsolation.h/.cpp
    SurvivalPolicy.h/.cpp       # AI/combat suppression, health, kill and magic guards
  forms/
    FormLookup.h/.cpp
```

These are ownership boundaries, not a requirement to create every file in the
first commit. Keep public headers narrow. A `.cpp` should include the smallest
headers it needs; avoid making every module include `NPCSpawner.h` or using the
precompiled header as an implicit interface. Keep private engine helpers and
constants in their owning `.cpp` or an internal header. Use namespaces and
plain C++23 value/state types here; virtual interfaces or a module registry add
indirection without a current need for runtime substitution.

The `sync/` directory contains features seen on the wire and their game-thread
state. The `actor/` directory contains Skyrim-specific actor mechanisms. For
example, `CombatSync` decides *when* a network attack may play; `CombatIsolation`
prevents local world combat with the actor. Likewise `AppearanceSync` owns a
peer's appearance revision; `ActorSpawner` owns actor allocation, using the
base that appearance selects.

### Naming transition

Use `Actor` for project modules and `RemoteActor` for the replicated actor
instance or handle. Keep `RE::Actor` qualified when referring to Skyrim's
engine type, so `RemoteActor` and `RE::Actor` cannot be mistaken for each other.
`PeerState` continues to mean the network player's complete receive state;
it is not the actor itself. During migration, old `NPCSpawner`, `ControlledNPC`,
and `RemotePuppet` APIs can be forwarding adapters, but remove them after
callers have moved to `ActorSpawner`, `RemoteActor`, and `PeerState`. New code
should not introduce `Puppet` names. Existing wire names and log messages can
stay until a separate change where changing them is intentional and testable.

## State and interface shape

The coordinator owns one `std::unordered_map<PlayerID, PeerState>` on the
bound main-loop thread. `PeerState` is composition, not a new collection of
methods implementing every feature:

```cpp
struct PeerState {
    Network::PlayerTransform latestCheckpoint;
    std::uint64_t lastSequence{ 0 };
    std::uint64_t appliedSequence{ 0 };
    ActorLifecycleState lifecycle; // optional RemoteActor, spawn space/retry
    MovementSync::RemoteState movement;
    AnimationSync::RemoteState animation;
    FurnitureSync::RemoteState furniture;
    CombatSync::RemoteState combat;
    AppearanceSync::RemoteState appearance;
    EquipmentSync::RemoteState equipment;
};
```

Only include fields a module actually needs. An empty state type need not be
stored. The remote actor handle and ready flag have one owner in `ActorLifecycleState`;
modules receive a borrowed actor view for the current pass. The actor handle
must be checked again before dereferencing it, as the current code does. The
current `NPCSpawner::AnimationState` fields belong in `ActorAnimation::State`;
furniture fields in `ActorFurniture::State`; transform targets in
`ActorMovement::State`. Do not copy them into both `PeerState` and the actor.

Each sync module exposes a named frame handle, with narrow non-frame methods
for accepting received data and lifecycle events. The interface can use free
functions in a feature namespace:

```cpp
void MovementSync::SyncMovement(LocalFrame& local, float deltaSeconds);
void MovementSync::SyncMovement(RemoteFrame& remote, float deltaSeconds);
void FurnitureSync::SyncFurniture(LocalFrame& local, float deltaSeconds);
void FurnitureSync::SyncFurniture(RemoteFrame& remote, float deltaSeconds);
void AppearanceSync::SyncAppearance(LocalFrame& local, float deltaSeconds);
void AppearanceSync::SyncAppearance(RemoteFrame& remote, float deltaSeconds);
void EquipmentSync::SyncEquipment(LocalFrame& local, float deltaSeconds);
void EquipmentSync::SyncEquipment(RemoteFrame& remote, float deltaSeconds);
void AnimationSync::SyncAnimation(LocalFrame& local, float deltaSeconds);
void AnimationSync::SyncAnimation(RemoteFrame& remote, float deltaSeconds);
void CombatSync::SyncCombat(LocalFrame& local, float deltaSeconds);
void CombatSync::SyncCombat(RemoteFrame& remote, float deltaSeconds);
```

`LocalFrame` and `RemoteFrame` are short-lived context views, not global service
objects. They provide a player/peer, the current checkpoint, typed feature
state, and only the small cross-feature results needed for the current pass
(for example `handsSettled`, `furniturePlaying`, `attackPlaying`,
`animationSettled`, and `translationState`). A function that has two distinct
stages can use explicit names such as `CaptureLocalAttack` and
`DispatchRemoteAttack` instead of hiding both in a single call. Keep the named
`Sync...` entry points as the module's frame surface and document its stage.
Avoid a shared mutable `SyncContext` that lets every module edit everything.

| Producer | Consumer | Explicit result or request |
| --- | --- | --- |
| Equipment | Animation and combat | Hands settled; bounded permission to show weapon draw |
| Animation | Furniture and combat | Graph settled, jump/weapon transition state, attack dispatch readiness |
| Furniture | Animation, combat, movement | Native interaction owns graph/transform; suppress or defer these updates |
| Combat | Equipment and checkpoint publisher | Captured local attack forces equipment flush; action uses published checkpoint sequence and graph epoch |
| Appearance | Lifecycle | Selected actor base and respawn request |
| Lifecycle | All remote modules | Borrowed live actor and spawn/retire notifications |

Keep these results as typed return values or narrow query functions. A feature
must not reach into another feature's state fields to infer them.

The transport remains a data-only boundary: Socket.IO callbacks parse and
enqueue plain values; the main-loop pass drains once and performs all Skyrim
object mutations. `ActorRegistry` exposes small thread-safe query/observation
operations for hook callbacks on worker threads. It must preserve registration
before hooks can recognize a new actor, removal before deletion, and the
current locking/atomic behavior. Worker callbacks must not use `PeerState` or
mutate the graph.

## Frame sequence and dependencies

The coordinator has an explicit order rather than an unordered module loop.
The pseudocode below is the current behavior expressed as stages; a refactor
must preserve the exact guards inside each stage.

```text
MainLoopHook::Thunk
  call original Skyrim function
  validate QFrameAnimTime and bind/verify authoritative thread
  SyncCoordinator::OnFrame(player, deltaSeconds)
    changes = Network::DrainRemotePlayerChanges() once
    apply disconnects, then newest transforms, appearances, equipment, actions
    if no local player: return
    CombatSync capture local normal-attack edge
    AnimationSync observe local graph epoch
    EquipmentSync local capture/publish; flush if attack captured
    when the 0.05 s checkpoint publisher is due and connected:
      MovementSync sample local position, heading, speed, direction and space
      AnimationSync sample local locomotion/jump/weapon stance
      FurnitureSync sample local occupied reference
      publish one complete player:update checkpoint
      CombatSync publish eligible captured attack(s) after that checkpoint
    AppearanceSync local capture/publish
    if no local cell or player base: return
    for each remote peer (current map iteration semantics):
      AppearanceSync advance timers and request respawn if needed
      lifecycle retire for appearance/space change; age/clear attack history
      if incompatible coordinate space: continue
      MovementSync apply a new transform target
      EquipmentSync reconcile current remote actor
      FurnitureSync apply immediate exit/change target
      AnimationSync stage grounded/airborne/weapon state if allowed
      FurnitureSync start first entry when animation and attack permit
      CombatSync attempt the queued attack if allowed
      ActorRuntime update one remote actor in its existing internal order
      AppearanceSync apply post-3D presentation
      if actor absent: select base and attempt spawn; initialize module state
```

The local checkpoint is one coherent value: position, speed/direction,
animation state, occupied furniture, graph epoch, and sequence are sampled for
the same publication. Keep `player:update` before `animation:action`, and keep
equipment emission before the attack it enables. A new stage must not publish
a partial checkpoint. `Snapshot` can collect fields, but the publish cadence
and sequence stay with a single publisher.

Within `ActorRuntime::Update`, preserve the current `UpdateControlledNPC`
order: validate handle; advance attack playback; reassert remote-replica policy;
sanitize local combat; recover graph if needed; wait for 3D/controller/graph;
enable animation firewall; advance an active native furniture lifecycle;
apply graph state; gate translation on settlement; attempt deferred furniture
entry; then apply heading and direct movement. Furniture owns position, heading,
and graph while active. Animation state settling gates movement; the existing
landing and airborne exceptions stay intact. Equipment hand reconciliation
gates first weapon draw and attack dispatch. On disappearance, clear transient
attacks and retry spawning from the latest checkpoint.

Transport acceptance retains the current independent policies: transforms
are latest wins by sequence, appearance/equipment are latest wins by revision,
actions are ordered and bounded by sequence/checkpoint/graph epoch, and a
disconnect clears all state for that player. Received appearance or equipment
may precede a transform; neither alone spawns an actor. A coordinate-space or
appearance change retires the actor but retains the latest reconstructable
state. Furniture occupancy is reconstructable; historical attacks are not.

## Dependencies and compatibility

The dependency direction is `runtime -> sync -> actor` for game-thread work;
`sync -> network/protocol` for data; `actor -> protocol` only for value types.
`network` and `protocol` must not include Skyrim headers. The engine hook
registration in `runtime` calls installers in `actor`; those installers own
their detours and event sinks. Keep the present trampoline budget, hook sites,
install order, data-loaded registration, and runtime guards while moving them.
The existing `main.cpp` should remain a small SKSE entry point.

Do not change Socket.IO event names, field names, sequence/revision semantics,
server validation, or recording/replay formats as part of this refactor. Keep
vanilla graph events and behavior as the baseline. Equipment and appearance
form resolution must keep their existing vanilla fallback for mismatched
modpacks; furniture and cell/worldspace FormIDs currently depend on matching
load orders, so retain that limitation instead of silently inventing a new
resolution rule during a structural move. Preserve the remote actor's isolation
from local damage, magic, hostility, and combat decisions.

Frame durations remain seconds. In moved code keep each existing elapsed-time
constant and its exact retry/fallback condition. New time-based logic must
accumulate clamped delta seconds as `kMaximumFrameTime` does; an attempt cap
also needs a minimum elapsed interval. The existing pass counters used for
graph readiness across main-loop updates or diagnostic log throttling remain
pass counters, with comments explaining their meaning. Review any old raw
delta use separately; changing it in a move-only commit can change behavior.

## Migration plan

1. **Record a behavior baseline.** Inventory the current public APIs, hook
   addresses/order, trampoline size, wire fields, timing constants, and log
   messages. Use `latest.log` first if investigating a reproduced mismatch.
   Capture representative live or replay traces on the supported Skyrim
   runtime before edits.
2. **Extract the boundaries without altering logic.** Move protocol DTOs and
   form lookup, then isolate `MainLoopHook`, plugin startup, game-thread binding,
   and `SyncCoordinator`. Keep adapters to old `NPCSpawner` and
   `PlayerAppearance`/`PlayerEquipment` APIs so this step is mostly relocation.
3. **Extract remote actor engine mechanisms.** Move registry/firewall/isolation and
   policy, then spawning/readiness, movement, animation, and native furniture.
   Preserve each engine call order and the `UpdateControlledNPC` behavior behind
   a temporary forwarding API until all call sites migrate. Move state fields
   once, with one owner for each lifetime.
4. **Extract sync modules one at a time.** Start with appearance/equipment
   wrappers, then movement/checkpoint capture, animation, furniture, and combat.
   After each move replace one coordinator block with its named `Sync...` call.
   Delete forwarding APIs only after all consumers have changed. Keep the
   coordinator's stage order visible in one short file.
5. **Remove obsolete files and update XMake.** `xmake.lua` currently uses
   `src/**.cpp` and `src/**.h`, so verify that nested sources are included by
   those globs; keep `src` as the include root and `src/pch.h` configured.
   Avoid adding a second build system or moving generated/vendor sources.

Make each step reviewable and behavior preserving. Do not combine a relocation
with retuning snapshot rate, interpolation, animation retries, protocol
semantics, or engine hook sites.

## Verification and completion criteria

- Review a before/after call-order trace for startup hooks, one local frame,
  network drain, remote frame, spawn, recycle, and deletion. Check that actor
  registration and unregistration still bracket the reference lifetime.
- Check local publishing at the existing 20 Hz cadence, connection republish,
  attack checkpoint/epoch ordering, sequence/revision rejection, bounded retry
  timing, and disconnect cleanup. Existing `server/tests` and replay fixtures
  can verify unchanged wire behavior where they cover it.
- On Skyrim, compare a baseline and refactored run for late join, reconnect,
  interior/exterior transitions, movement/jump, drawn and sheathed weapons,
  attack after equipment swap, furniture entry/exit, appearance respawn, and
  local damage/magic/combat isolation. Watch graph recovery and custom-mod
  fallback paths in `latest.log`.
- Build the C++ plugin with XMake under Wine on a supported host and run it in
  Skyrim. Do not treat a Linux-host build as a valid gate for this project.

The restructuring is complete when `FrameUpdate.cpp` and `NPCSpawner.cpp`
have been replaced by the narrow coordinator, hook, spawn, and feature owners
above; the new code uses `Actor` naming instead of `Puppet` naming; every
feature has a visible frame entry point; one module owns each piece of
persistent state; and the behavior checks match the baseline.

## Implementation notes

The structural move is implemented. Where it differs from the proposal above:

- Local movement, animation, and furniture have no independent frame step;
  they are samplers (`Sample...`) called by `sync/CheckpointPublisher`, which
  replaces the proposed `Snapshot.h` and owns cadence and sequence. Attack
  capture, graph-epoch observation, and the attack backlog run every pass.
- Furniture and combat have two remote stages separated by animation staging
  (`SyncFurniture`/`SyncFurnitureEntry`, `AgeRemoteActions`/`SyncCombat`).
  Cross-feature results (`handsSettled`, `readyForWeaponDraw`,
  `furniturePlaying`, `attackPlaying`) are explicit arguments.
- Spawn/retire lives in `sync/ActorLifecycle`. `PeerState::appliedSequence`
  stays top-level; movement, animation, and furniture need no peer state.
- `RemoteActor` (`actor/RemoteActor.h`) composes `ActorMovement::State`,
  `ActorAnimation::State` (heap-allocated: its atomics are not movable), and
  `ActorFurniture::State`. `furnitureAnimationOwned` moved to furniture.
- `actor/` depends on `runtime/GameThread` for its thread guard.
- `ActorRegistry` preserves each original lock scope as one operation. Pure
  event-name classification in the graph input hook runs outside the lock.
- Log messages and `GameThread::Require` operation names are unchanged, so
  logs still say "puppet" and the old function names.
