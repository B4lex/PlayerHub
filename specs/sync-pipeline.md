# Sync Pipeline

## Status

Implemented, and every feature in the other system specs runs through it in
game on Skyrim AE 1.7.104. The split into `runtime/`, `sync/`, and `actor/` was
a behavior-preserving move of the former `FrameUpdate.cpp` and `NPCSpawner.cpp`.
Spawn throttling (`[Performance] SpawnsPerSecond`) and save exclusion are not
separately checked in game.

## Scope

The pipeline owns the Skyrim main-loop hook and the game-thread binding, the
fixed order of one synchronization pass, every peer's receive state, the local
checkpoint publisher, the player's world presence, and the remote actor
lifecycle: spawn, retirement, and respawn.

It leaves engine mechanics of the actor to [Remote actor](remote-actor.md),
each feature's rules to its own system, the transport to
[Network](network.md), and the configuration to
[Configuration](configuration.md).

## Components

| Module | Responsibility |
| --- | --- |
| `runtime/PluginStartup` | Loads the configuration, allocates the trampoline, installs every hook, and handles SKSE messages |
| `runtime/MainLoopHook` | The one main-loop detour; binds the game thread and runs a pass |
| `runtime/GameThread` | `Bind`, `IsAuthoritative`, `Require`: the engine-mutation thread guard |
| `runtime/GameSession` | Whether a game is loaded, set by `PresenceSync` |
| `runtime/PluginTime` | Plugin time per pass, averaged each second ([UI overlay](ui-overlay.md#plugin-time)) |
| `sync/SyncCoordinator` | One frame entry, the stage order, the single network drain, and the peer map |
| `sync/PeerState` | A peer's complete receive state, composed of feature-owned members |
| `sync/SyncFrame` | `LocalFrame`, `RemoteFrame`, and `CheckpointUpdate` views for one pass |
| `sync/CheckpointPublisher` | Checkpoint cadence and sequence; calls every feature's local sampler |
| `sync/ActorLifecycle` | Spawn, retirement, stranded and lost actors, spawn throttling, save exclusion, the mount beside its rider |
| `sync/presence/PresenceSync` | `Enter` and `Exit`, and the game session |

`sync/` holds features seen on the wire and their game-thread state; `actor/`
holds Skyrim actor mechanisms. For example, `CombatSync` decides when a network
attack may play, and `CombatIsolation` keeps the local world from fighting the
actor. `AppearanceSync` owns a peer's appearance revision, and `ActorSpawner`
allocates the actor from the base that appearance selects.

Dependencies run `runtime -> sync -> actor` for game-thread work,
`sync -> network/protocol` for data, and `actor -> protocol` for value types only, plus
`actor -> runtime/GameThread` for the thread guard. `network/` and `protocol/`
include no Skyrim headers. `runtime` calls the installers in `actor`, which own
their detours and event sinks. `main.cpp` stays a small SKSE entry point.

`Actor` names project modules and `RemoteActor` the replicated actor instance
(`actor/RemoteActor.h`); `RE::Actor` stays qualified. Log messages and
`GameThread::Require` operation names kept their older wording ("actor",
`UpdateControlledNPC`, `SpawnControlledNPC`) so runs stay comparable.

## Threading

- Skyrim engine reads and mutations run only on the thread of the main-loop
  hook. `GameThread::Bind` records the first thread that runs the hook, and
  every pass after that must run on it. A hook that migrates threads stops the
  pass. `RE::Main::threadID` is not used: a runtime comparison showed that it
  did not identify the thread running the pass.
- `GameThread::Require` guards each public remote-actor operation that
  dereferences an engine object. A caller on another thread is refused and
  logged once.
- `PlayerCharacter::UpdateAnimation` and tasks queued through
  `SKSE::TaskInterface::AddTask` ran on changing worker threads (seen in logs),
  so neither is a safe place for actor or graph mutation. An earlier version
  that wrote `Actor::actorState1` movement bits from the animation hook left
  invalid engine state that crashed later in FaceGen, without a plugin frame
  on the stack. The plugin never writes engine-owned movement-state fields.
- Network callbacks only validate and queue plain values behind a lock. Hook
  callbacks on worker threads only query `ActorRegistry`, which keeps its
  locking operations small and never touches `PeerState` or a graph.

## Frame sequence

`MainLoopHook` replaces the call at ID 36564 + 0x6E (ID 40438; SE ID 35565 +
0x53). The thunk calls the original first, then, inside a `PluginTime::Scope`:

1. binds or checks the game thread;
2. reads `RE::Main::QFrameAnimTime()`. Only a finite positive delta runs
   `SyncCoordinator::OnFrame`, so nothing advances in the main menu or while
   paused;
3. calls `Network::UpdateConnectionMonitor` with that delta, or zero when the
   frame did not advance.

The original call reaches Skyrim's per-frame actor movement jobs, so the engine
movement update runs before the pass and the network transform is the last
write before render ([Remote actor](remote-actor.md#frame-order)).

`SyncCoordinator::OnFrame`:

```text
restore save eligibility after a save
DefaultOutfit::Refresh                    # re-resolve a changed [DefaultOutfit]
ApplyNetworkChanges                       # the one drain
ActorLifecycle::RefillSpawnAllowance
if no player: return
YieldActivatedFurniture                   # queued local furniture activations
if PresenceSync::SyncPresence (in world):
    CombatSync::CaptureLocalAttack
    CombatSync::CaptureLocalHandPresentation
    AnimationSync::ObserveLocalGraphEpoch
    EquipmentSync::SyncEquipment           # flushed when an attack or presentation started
    CombatSync::QueueLocalAttacks
    CheckpointPublisher::PublishLocalCheckpoint
        disconnected: discard queued attacks
        published: CombatSync::PublishLocalAttacks after the checkpoint
    AppearanceSync::SyncAppearance
else: discard captured attacks
if no local cell: return
for each peer: SyncPeer
```

`SyncPeer` for one remote player:

```text
AppearanceSync advance timers; retire on a new appearance revision
retire on a coordinate-space change
CombatSync::AgeRemoteActions
not in the local space: clear actions, stop
DeathSync::SyncDeath                       # a killed actor keeps only handle and look
ActorLifecycle::RetireWhenStranded
FurnitureSync yield check                  # hide an actor that would block furniture
MountSync::SyncMount                       # riding: equipment, rider update, look only
otherwise:
    MovementSync::SyncMovement             # new transform target
    EquipmentSync::SyncEquipment           # -> handsSettled, readyForWeaponDraw
    FurnitureSync::SyncFurniture           # exit or target change -> furniturePlaying
    AnimationSync::SyncAnimation           # unless an attack or furniture owns the graph
    FurnitureSync::SyncFurnitureEntry
    CombatSync::SyncCombat, SyncHandPresentation   # unless furniture owns the actor
    AnimationSync::GetTranslationState
    ActorRuntime::Update
    AppearanceSync::ApplyActorPresentation
no actor: end a vacated furniture yield, ActorLifecycle::SpawnWhenDue
```

Cross-feature results (`handsSettled`, `readyForWeaponDraw`, `furniturePlaying`,
`attackPlaying`, the translation state) are passed as explicit arguments. A
feature never reads another feature's state fields to infer them.
`ActorRuntime::Update` then applies the engine side in its own order
([Remote actor](remote-actor.md#update-order)).

## Local publishing

`CheckpointPublisher` owns the checkpoint cadence and sequence. It publishes
every 0.05 s of accumulated frame time while connected, keeping the remainder so
the average rate holds, and drops whole missed intervals instead of sending a
burst. A pass without a parent cell publishes nothing. The sequence wraps
before `INT64_MAX`. `Network::PublishLocalPlayerTransform` sends the sample's
context (cell, worldspace, graph epoch, furniture) first whenever it differs
from the last one sent since `Enter`, then the quantized checkpoint
([Network](network.md#checkpoint-and-context)).

One checkpoint is one coherent sample: `MountSync::SampleLocalMount`,
`MovementSync::SampleLocalMovement` (from the mount while riding),
`AnimationSync::SampleLocalAnimation`, `FurnitureSync::SampleLocalFurniture`,
`CombatSync::SampleLocalHandPresentation`, and `DeathSync::SampleLocalDeath`,
with the graph epoch observed this pass. No stage publishes a partial
checkpoint. The checkpoint always precedes the `Action` anchored to it, and a
weapon swap is published before the attack or presentation that uses it.

Appearance and equipment publish on their own schedules and republish on every
new connection generation ([Network](network.md#latest-wins-publishers)).

## World presence

`PresenceSync` tells the server when the local player is in the world:

- In the world means no loading screen or main menu is open and the player has
  a parent cell and loaded 3D. The first such pass also starts the game session
  (`GameSession::SetInGame(true)`).
- On each connection generation the player starts out of the world, so
  `Enter` is sent again once it is in the world.
- `Exit` is sent once when a loading screen or the main menu opens
  (menu sink), when a save starts loading (`kPreLoadGame`), or when a pass finds
  the player out of the world. The main menu also ends the game session.
- Attacks captured while out of the world are discarded, so they cannot anchor
  to the first checkpoint after returning.

## Receive state

`SyncCoordinator` owns one `std::unordered_map<Multiplayer::PeerSlot, PeerState>`
keyed by the server's slot for the peer. `PeerState` holds the latest checkpoint, its sequence,
the sequence whose transform the current actor last received, and the
feature-owned `combat`, `lifecycle`, `appearance`, and `equipment` members.
Movement, animation, and furniture keep their state on the `RemoteActor`
instead.

`ApplyNetworkChanges` drains everything the network thread queued, once per
pass:

1. A lost or replaced connection removes every peer: slots belong to one
   connection, so the server will never update or report them again.
   Reconnecting rebuilds peers from the server's `PeerEnter` messages.
2. A `PeerLeave` for any reason but `DISCONNECTED` (the peer left the world,
   went out of range, or was excluded) removes the actor and forgets the
   checkpoint, sequence, and combat state, but keeps appearance and equipment
   until a `PeerEnter` places the player again. Leaves apply first, since
   changes drained with them arrived after them.
3. `PeerLeave` with `DISCONNECTED` removes the actor and all state.
4. Checkpoints are latest-wins by sequence, each already merged with the
   peer's context on the transport thread. A `PeerEnter` delivers its stored
   checkpoint the same way, so a paused peer appears at once. A stale one is
   logged and ignored.
   An accepted one is shown to furniture, animation, and combat
   (`OnCheckpointAccepted`) before it replaces the previous one. Furniture
   resolves its reference into the local load order there, once per change.
5. Appearance and equipment are latest-wins by revision and may arrive before
   any checkpoint. Neither alone spawns an actor.
6. Actions are ordered and bounded by the combat system.

Furniture occupancy, weapon stance, and every other checkpoint field are
reconstructable; historical attacks are not.

## Coordinate spaces

A checkpoint's context carries `cell_id` and `worldspace_id`. Exterior checkpoints with the
same non-zero worldspace share the exterior grid's coordinates and stay in sync
across cell boundaries. An interior has worldspace zero and needs an exact cell
match, because its coordinates are local to it. Both need matching load orders
on every peer.

## Actor lifecycle

`ActorLifecycle` creates and retires the peer's `RemoteActor` and, while the
player rides, its mount ([Native interactions](native-interactions.md#mounts)).

- **Spawn.** An actor is created directly at the newest checkpoint's position
  and heading once the player is in the local space, the spawn location is
  loaded (`ActorSpawner::IsSpawnLocationLoaded`: an exterior cell containing
  the position, or the local player's interior), and
  `AppearanceSync::GetSpawnBase` has a base. It is never created at the local
  player first. A failed spawn is retried after 1 s. A spawn records the cell
  and worldspace it was created in.
- **Throttling.** `[Performance] SpawnsPerSecond` (default 20, `0` unlimited)
  bounds actor and mount spawns. `RefillSpawnAllowance` adds the rate times the
  frame time once per pass, never saving up more than one pass's share, and a
  pass may always start one spawn. A held-back spawn accumulates its wait, and
  a due spawn starts only once it has waited as long as the longest spawn held
  back in the previous pass, so a fixed peer order cannot starve later peers.
- **Retirement** removes the actor and its mount, clears queued actions and the
  applied sequence, and keeps the latest reconstructable state for a respawn.
  Reasons, each logged as `Recycling remote player actor ...`: a new appearance
  revision, a coordinate-space change, fast travel, revival, dismount, a lost
  mount, and death while riding.
- **Fast travel** within one space keeps the coordinate space, so it is found by
  displacement: an actor more than 4096 units (`kRelocationDistance`) from its
  checkpoint is retired, and so is one without 3D for 2 s whose target location
  is not loaded, since its own cell stayed unloaded and it cannot regain 3D.
- **A lost handle** schedules a respawn after the retry delay.
- **Removal** for a `PeerLeave` or a lost connection
  removes the actor and mount without a respawn.
- **Removing a reference** releases its furniture occupancy and marker
  reservations first (a reservation that outlives its actor makes Skyrim refuse
  every entry point to the replacement), unregisters it, then disables and
  deletes it.

### Saves

Remote actors belong to the session, not the save. On `kSaveGame`, dispatched
before the engine writes, every live actor's reference is excluded from the
save, and the next pass after saving restores it. A removed reference is
excluded permanently: deleting a reference does not retire the change form the
save writer keeps for it, and that record carries the runtime actor base, which
is never serialized. On load it bound to whichever dynamic form received the
same ID, and the load crashed.

## Timing

Durations accumulate the clamped frame delta in seconds
(`ActorConstants::kMaximumFrameTime`) and use `...Seconds` constants. Pass
counters remain only where the logic means engine update passes (graph
readiness across passes) or for log throttling, with a comment saying so.

## Known limitations

- Cell and worldspace form IDs require matching load orders.
- Interior and worldspace changes despawn and respawn the actor; there is no
  presentation through loading screens.
- Transforms chase the newest checkpoint. There is no timestamped
  interpolation buffer, extrapolation, or latency compensation.
- There is no authentication, lobby, persistent identity, or server-authoritative
  movement.
