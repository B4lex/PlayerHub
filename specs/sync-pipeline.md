# Sync Pipeline

## Status

Implemented, built on Windows, and run in game on Skyrim AE 1.7.104; every
feature in the other system specs runs through it. Spawn throttling
(`[Advanced] SpawnsPerSecond`) and save exclusion are not separately checked
in game. The [vanilla intro](#vanilla-intro) hold has not run in game; its
stages and the `kNewGame` payload were read from the 1.7.104 data files and
`skse64_1_7_104.dll` ([Engine assumptions](#engine-assumptions)).

[Checkpoint playout](#checkpoint-playout) ran in game against a server with
a 20 Hz tick: remote walking and running moved at an even pace. Jumps,
attacks, furniture, mounts, a local pause, and a peer resuming from idle were
not separately checked with it. `CheckpointPlayout` also ran in a Linux
simulation against stubbed engine types: a 60 fps source publishing at 20 Hz,
a server relaying latest-wins at 10, 20, or 40 Hz with idle suppression,
network jitter with delay spikes, and receivers at 30 to 144 fps. At a 20 Hz
tick the per-pass speed deviation of a following actor was about 7% against
about 23% when chasing the newest checkpoint, and passes without movement
0.3% against 3%, for about 50 ms more delay.

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
| `runtime/PluginStartup` | Sizes the trampoline that `SKSE::Init` allocates, loads the configuration, installs every hook, and handles SKSE messages |
| `runtime/MainLoopHook` | The one main-loop detour; binds the game thread and runs a pass |
| `runtime/GameThread` | `Bind`, `IsAuthoritative`, `Require`: the engine-mutation thread guard |
| `runtime/GameSession` | Whether a game is loaded, set by `PresenceSync` |
| `runtime/VanillaIntro` | Whether the vanilla Helgen intro holds the connection ([Vanilla intro](#vanilla-intro)) |
| `runtime/RemoteActorActivation` | The local player's activation of a remote actor or mount, on its worker thread ([UI overlay](ui-overlay.md#player-menu-in-the-world)) |
| `runtime/PluginTime` | Plugin time per pass, averaged each second ([UI overlay](ui-overlay.md#plugin-time)) |
| `runtime/PlayerGraphEventHook` | The local player's accepted graph input events, for [beast actions](combat-presentation.md#beast-actions) |
| `sync/SyncCoordinator` | One frame entry, the stage order, the single network drain, and the peer map |
| `sync/PeerState` | A peer's complete receive state, composed of feature-owned members |
| `sync/SyncFrame` | `LocalFrame`, `RemoteFrame`, `RemotePose`, and `CheckpointUpdate` views for one pass |
| `sync/CheckpointPlayout` | A peer's received checkpoints, its playback clock, the played checkpoint, and the interpolated pose |
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
(`actor/RemoteActor.h`); `RE::Actor` stays qualified. Some log messages and
`GameThread::Require` operation names use other words ("actor",
`UpdateControlledNPC`, `SpawnControlledNPC`), kept so logs of different
builds stay comparable.

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
  so neither is a safe place for actor or graph mutation. Writing
  `Actor::actorState1` movement bits from the animation hook left invalid
  engine state that crashed later in FaceGen, without a plugin frame on the
  stack, so the plugin never writes engine-owned movement-state fields.
- Network callbacks only validate and queue plain values behind a lock. Hook
  callbacks on worker threads only query `ActorRegistry`, which keeps its
  locking operations small and never touches `PeerState` or a graph, or, for
  the local player's graph input, queue a beast action behind
  `ShapeshiftSync`'s own lock.

## Frame sequence

`MainLoopHook` replaces the call at ID 36564 + 0x6E (ID 40438; SE ID 35565 +
0x53). The thunk calls the original first, then, inside a `PluginTime::Scope`:

1. binds or checks the game thread;
2. reads `RE::Main::QFrameAnimTime()`;
3. calls `VanillaIntro::Update` with that delta, or zero when the frame did
   not advance. It runs before the pass that may start the game session, so
   the overlay never sees a started session without the intro's hold;
4. runs `SyncCoordinator::OnFrame` only for a finite positive delta, so
   nothing advances in the main menu or while paused;
5. calls `Network::UpdateConnectionMonitor` with that delta, or zero when the
   frame did not advance.

The original call reaches Skyrim's per-frame actor movement jobs, so the engine
movement update runs before the pass and the network transform is the last
write before render ([Remote actor](remote-actor.md#frame-order)).

`SyncCoordinator::OnFrame`:

```text
NameplateSync::Clear                      # an early return leaves no plates
restore save eligibility after a save
DefaultOutfit::Refresh                    # re-resolve a changed [DefaultOutfit]
ApplyNetworkChanges                       # the one drain; trade events queued
ServePlayerDetails                        # the open player menu's race and level
PlayCheckpoints                           # each peer's due checkpoints play
ActorLifecycle::RefillSpawnAllowance
if no player: return
YieldActivatedFurniture                   # queued local furniture activations
OpenActivatedPlayerMenu                   # the latest activated remote actor's player menu
inWorld = PresenceSync::SyncPresence
if inWorld:
    ShapeshiftSync::ObserveLocalForm       # the local beast form, for the graph input hook
    CombatSync::CaptureLocalAttack         # dropped in a beast form
    CombatSync::CaptureLocalHandPresentation
    AnimationSync::ObserveLocalGraphEpoch
    AnimationSync::ObserveLocalMovementInput   # stop hold timer, input history
    EquipmentSync::SyncEquipment           # flushed when an attack or presentation started
    CombatSync::QueueLocalAttacks
    ShapeshiftSync::QueueLocalActions      # beast actions the graph input hook captured
    CheckpointPublisher::PublishLocalCheckpoint
        disconnected: discard queued attacks and beast actions
        published: CombatSync::PublishLocalAttacks, then ShapeshiftSync::PublishLocalActions
    AppearanceSync::SyncAppearance
else: discard captured attacks and beast actions
TradeSync::Sync                           # trade events, overlay commands, cancels, offers, a commit
if no local cell: return
PrecisionIsolation::Update                # reassert after the player's Havok world changes
for each peer: SyncPeer
NameplateSync::Update                     # head nodes, line of sight, the plates the overlay draws
RemoteLights::Update                      # with a world camera: the remote light budget
```

`SyncPeer` for one remote player:

```text
AppearanceSync advance timers; retire on a new appearance revision, or else on a new form
retire on a coordinate-space change
CombatSync::AgeRemoteActions, ShapeshiftSync::AgeRemoteActions
not in the local space: clear attacks and beast actions, stop
DeathSync::SyncDeath                       # a killed actor keeps only handle and look
ActorLifecycle::RetireWhenStranded
FurnitureSync yield check                  # hide an actor that would block furniture
MountSync::SyncMount                       # riding: equipment, rider update with weapon state, look only
otherwise:
    MovementSync::SyncMovement             # played checkpoint's movement, then the pose
    EquipmentSync::SyncEquipment           # -> handsSettled, readyForWeaponDraw
                                           # a beast: SyncBeastEquipment, both true
    FurnitureSync::SyncFurniture           # exit or target change -> furniturePlaying
    AnimationSync::SyncAnimation           # unless an attack or furniture owns the graph
    FurnitureSync::SyncFurnitureEntry
    unless furniture owns the actor:
        a beast: ShapeshiftSync::SyncBeast # beast actions, levitation, casts, Mist Form and bats
        otherwise: CombatSync::SyncCombat, SyncHandPresentation
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
context (cell, worldspace, graph epoch, furniture, shapeshift) first whenever it differs
from the last one sent since `Enter`, then the quantized checkpoint
([Network](network.md#checkpoint-and-context)).

One checkpoint is one coherent sample: `MountSync::SampleLocalMount`,
`MovementSync::SampleLocalMovement` (from the mount while riding),
`AnimationSync::SampleLocalAnimation`, `FurnitureSync::SampleLocalFurniture`,
`CombatSync::SampleLocalHandPresentation`, `DeathSync::SampleLocalDeath`, and
`ShapeshiftSync::SampleLocalVampireLord`, with the graph epoch and the local
form observed this pass. No stage publishes a partial
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
  the player out of the world. The main menu also ends the game session. The
  menu sink reports the exit as soon as the menu opens, before the main loop
  may stall on the load; since it and the SKSE message run outside the
  main-loop pass, the published state is kept under a lock.
- Attacks captured while out of the world are discarded, so they cannot anchor
  to the first checkpoint after returning.

## Vanilla intro

`VanillaIntro` tells the [connection](network.md#vanilla-intro-hold) and the
overlay whether the vanilla Helgen intro is playing, from the state of the
quest MQ101 (`Skyrim.esm` local form ID `0x0003372B`, looked up at
`kDataLoaded`). `IsPlaying` reads an atomic, so the overlay may call it;
`Update` sets it on the main loop.

- It holds while MQ101 is running (`TESQuest::IsEnabled`) below stage 160
  (`kPlayerControlStage`). Stage 160 unbinds the player, enables movement,
  and shows the move help after Alduin's knockdown (stage 140), the first
  point after the cart where the player plays freely. CommonLib's
  `IsRunning` is not used: it is also true for a stopped quest.
- A stopped, completed, or never started MQ101 never holds, so an alternate
  start, a console skip, or a mod that sets a later stage connects at once.
- The quest is checked only while the hold can change: from `kNewGame`, or
  from `kPostLoadGame` once a save's quest state is restored, each pass until
  the hold is off. `Update` then returns at its first check until the next
  game, so a game past the intro checks once per load. Between `kPreLoadGame`
  and `kPostLoadGame` nothing is checked, because the quest holds the
  previous game's or a partly loaded state, and the hold keeps its last
  value.
- At `kNewGame`, SKSE passes the quest the new game starts. When it is MQ101,
  the hold starts at once (`s_awaitingStart`), because the start may only be
  queued and the player could reach the world before MQ101 runs. It ends
  when MQ101 runs or completes, at `kPreLoadGame`, or after 5 s
  (`kStartWaitSeconds`) of clamped frame time in the game without MQ101
  running, logged as a start that did not happen. Any other start quest
  starts no wait, so an alternate start is not delayed.
- `IsPlaying` is false outside a game (`GameSession::IsInGame`), so the last
  game's hold does not reach the main menu without a per-pass menu lookup.
- Each check that ends with the hold off, and each change, is logged as
  `Vanilla intro hold on` or `off`, with MQ101's running flag and stage and
  whether the new-game wait is active. `kNewGame` logs whether its quest is
  MQ101.

## Receive state

`SyncCoordinator` owns one `std::unordered_map<Multiplayer::PeerSlot, PeerState>`
keyed by the server's slot for the peer. `PeerState` holds the peer's
`CheckpointPlayout`, the sequence whose checkpoint the current actor last
received, and the feature-owned `combat`, `shapeshift`, `lifecycle`,
`appearance`, `equipment`, `furniture`, and `nameplate` members. Movement and animation keep their state
on the `RemoteActor` instead. A `RemoteFrame` presents the played checkpoint,
its sequence, and the interpolated pose; features never see a checkpoint that
has not played.

`ApplyNetworkChanges` drains everything the network thread queued, once per
pass:

1. A lost or replaced connection removes every peer: slots belong to one
   connection, so the server will never update or report them again.
   Reconnecting rebuilds peers from the server's `PeerEnter` messages.
2. A `PeerLeave` for any reason but `DISCONNECTED` (the peer left the world,
   went out of range, or was excluded) removes the actor and resets the
   playout and combat state, but keeps appearance and equipment
   until a `PeerEnter` places the player again. Leaves apply first, since
   changes drained with them arrived after them.
3. `PeerLeave` with `DISCONNECTED` removes the actor and all state.
4. Checkpoints, each already merged with the peer's context and stamped with
   its arrival time on the transport thread, go into the peer's playout with a
   rising sequence. A stale one is logged and ignored. A `PeerEnter` delivers
   its stored checkpoint the same way.
5. Appearance and equipment are latest-wins by revision and may arrive before
   any checkpoint. Neither alone spawns an actor.
6. Actions are ordered and bounded by the combat system, and accepted against
   the newest received checkpoint, played or not.
7. Inspections go to `PlayerDetails::Accept`
   ([UI overlay](ui-overlay.md#player-menu)).
8. Trade events go to `TradeSync::QueueNetworkChanges`, which ends a trade
   whose connection was lost or whose events were dropped, and keeps the
   rest for `TradeSync::Sync` later in the pass, which needs the player
   ([Trade](trade.md)).

`PlayCheckpoints` then advances every peer's playout. Each checkpoint that
reaches its playback time is shown, in order, to furniture, animation,
combat, and shapeshift (`OnCheckpointPlayed`) before it replaces the played
one. Furniture
resolves its reference into the local load order there, once per change.

Furniture occupancy, weapon stance, and every other checkpoint field are
reconstructable; historical attacks are not.

## Checkpoint playout

Checkpoints leave the source every 0.05 s but arrive unevenly: the server
batches them once per send interval (`send_rate_hz`, 20) and replaces one that a newer
one overtook, sends far peers at `far_rate_hz`, and suppresses idle repeats
([Interest and delivery](interest-and-delivery.md#batching)), and the network
adds jitter. Following the newest checkpoint as it arrives would make the
actor alternate between catching up and waiting. `CheckpointPlayout` instead plays
each peer's checkpoints back at their sampled spacing, a little behind the
newest. The whole checkpoint waits, not only its transform, so movement state,
jumps, stance, hands, furniture, death, and the actions anchored to a
checkpoint stay with the position they were sampled at: actions dispatch only
once their anchor checkpoint has played ([Combat
presentation](combat-presentation.md#receiver-queue)).

- **Sender time.** Each checkpoint carries the source's monotonic clock in
  milliseconds modulo 16384 (`time`, [Network](network.md#checkpoint-and-context)).
  The receiver unwraps it into seconds, choosing the number of wraps that best
  matches the arrival gap.
- **Estimates**, updated per received checkpoint:
  - `transitFloor`, arrival minus sender time for the least delayed recent
    checkpoint: it drops to any lower value at once and rises by at most
    0.01 s per second of arrivals, so one late checkpoint cannot raise it;
  - `jitter`, the smoothed delay beyond the floor, rising with factor 0.25 and
    falling with 0.02, starting at 0.025 s;
  - `interval`, the smoothed sender-time spacing of gaps up to 0.25 s,
    starting at 0.05 s.
- **Delay** is `interval + 2 × jitter + 0.02 s` (a checkpoint arriving just
  after a drain waits a pass), clamped to 0.05–0.5 s. With a 20 Hz tick it is
  typically 0.12–0.16 s on top of the network latency.
- **Playback clock.** The target is `now − transitFloor − delay` on the local
  `steady_clock`. The clock advances with wall time and converges on the
  target at most 10% faster or slower than real time, so estimate changes
  never jump the actor. An error beyond 0.25 s, such as after the local game
  was paused, is jumped and logged (`Checkpoint playout resynchronized`). The
  clock starts at the target with the first checkpoint.
- **Play.** A checkpoint plays once the clock reaches its sender time; the
  first after a reset plays at once, so a peer appears without delay.
  Checkpoints played in the same pass each reach `OnCheckpointPlayed` in order.
  At most 16 wait; beyond that the oldest is dropped.
- **Pose.** Between the played checkpoint and the next waiting one, position
  and pitch interpolate linearly and heading along the shorter arc;
  `RemotePose.speed` is the segment's distance over its duration and
  `headingRate` its turn over its duration (0 once the segment is reached or
  without one). `sampledSpeed` interpolates the checkpoints' `speed` fields,
  and `direction` their `direction` along the shorter way around while both
  move and differ by less than 1/8 turn; otherwise it holds the played
  checkpoint's, so a larger change steps and is cross-blended
  ([Locomotion](locomotion.md#direction)), and an idle checkpoint's direction is
  never blended toward. A gap over
  0.25 s is not one stretch of movement: the player stood still, since idle
  repeats are not sent, or the connection stalled. The segment then starts
  only the next checkpoint's distance over its speed before it, at least one
  `interval` and at most the whole gap. Nothing interpolates across a
  coordinate-space change, a mount change, or more than 4096 units; the pose
  holds the played checkpoint until the next one plays. With nothing waiting,
  the pose holds the played checkpoint too: there is no extrapolation. When
  that lasts 0.02 s or more and began while the played checkpoint moved
  at 50 or more, above an idle player's drift, the end of the wait is logged (`Checkpoint playout ran
  dry`, with its duration and the estimates; the first 10, then every 100th).

Every step is constant work per peer per pass with no allocation after a
peer's first checkpoint.

## Coordinate spaces

A checkpoint's context carries `cell_id` and `worldspace_id`. Exterior checkpoints with the
same non-zero worldspace share the exterior grid's coordinates and stay in sync
across cell boundaries. An interior has worldspace zero and needs an exact cell
match, because its coordinates are local to it. Both need matching load orders
on every peer.

## Actor lifecycle

`ActorLifecycle` creates and retires the peer's `RemoteActor` and, while the
player rides, its mount ([Native interactions](native-interactions.md#mounts)).

- **Spawn.** An actor is created directly at the played checkpoint's position
  and heading once the player is in the local space, the spawn location is
  loaded (`ActorSpawner::IsSpawnLocationLoaded`: an exterior cell containing
  the position, or the local player's interior), and
  `AppearanceSync::GetSpawnBase` has a base for the played checkpoint's form. It is never created at the local
  player first. A failed spawn is retried after 1 s. A spawn records the cell
  and worldspace it was created in.
- **Throttling.** `[Advanced] SpawnsPerSecond` (default 20, `0` unlimited)
  bounds actor and mount spawns. `RefillSpawnAllowance` adds the rate times the
  frame time once per pass, never saving up more than one pass's share, and a
  pass may always start one spawn. A held-back spawn accumulates its wait, and
  a due spawn starts only once it has waited as long as the longest spawn held
  back in the previous pass, so a fixed peer order cannot starve later peers.
- **Retirement** removes the actor and its mount, clears queued attacks and the
  applied sequence, and keeps the latest reconstructable state for a respawn.
  Queued beast actions stay, since they may belong to the form the actor
  respawns in, and age out after 3 s. Reasons, each logged as `Recycling
  remote player actor ...`: a new appearance revision, a new
  [beast form](appearance-and-equipment.md#shapeshifts), a coordinate-space
  change, fast travel, revival, dismount, a lost mount, and death while
  riding.
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
is never serialized. On load it would bind to whichever dynamic form received
the same ID, which crashed the load.

## Timing

Durations accumulate the frame delta clamped to 0.1 s (`FrameTime::Clamp`)
and use `...Seconds` constants. Pass counters remain only where the logic
means engine update passes (graph readiness across passes) or for log
throttling (`ActorConstants::ShouldLogOccurrence`), with a comment saying so.
Two clocks are exceptions. The checkpoint playout follows the sender's clock,
so it runs on `steady_clock` and catches up after a local pause instead of
replaying it. `CheckpointPublisher` accumulates the unclamped delta, because
the elapsed time divides the distance travelled into the published speed.

## Engine assumptions

- [Vanilla intro](#vanilla-intro): the stage meanings come from the 1.7.104
  data files (`QF_MQ101_0003372B` Fragment_316 at stage 160:
  `SetRestrained(false)`, `EnablePlayerControls` with movement, the move help;
  Fragment_295 at stage 140: the knockdown idle). `skse64_1_7_104.dll`
  dispatches `kNewGame` after the call to `TESQuest::EnsureQuestStarted`
  (ID 25003) that it hooks returns, with that quest as `data`. The SE runtime's
  SKSE is assumed to send the same; the pointer is only compared, never
  dereferenced. That the player can reach the world before a queued MQ101
  start runs is not observed; the new-game wait only guards against it.
  `kPostLoadGame` is assumed to arrive before the loaded game's first pass
  with the player in the world, so a save in the intro starts its session
  with the hold already set.

## Known limitations

- Once the [vanilla intro](#vanilla-intro) hold is off, nothing reads MQ101
  until the next load or new game, so setting it back below stage 160 from
  the console does not hold the connection again.
- Cell and worldspace form IDs require matching load orders.
- Interior and worldspace changes despawn and respawn the actor; there is no
  presentation through loading screens.
- Remote players are shown the playout delay behind their network latency,
  typically 0.1–0.2 s in total. There is no extrapolation or latency
  compensation; when nothing waits to play, the actor stops at the played
  checkpoint and drains the gap once the next one arrives.
- The playout constants were tuned in simulation; in game they were only
  checked on one connection, not under real jitter or a far peer's 10 Hz rate.
- There is no lobby or server-authoritative movement.
