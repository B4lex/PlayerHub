# Sync Pipeline

## Status

The [vanilla intro](#vanilla-intro) new-game wait's end on the race menu
closing has not run in game with Alternate Perspective's start.

## Scope

The pipeline owns the Skyrim main-loop hook and the game-thread binding, the
fixed order of one synchronization pass, every peer's receive state, the local
checkpoint publisher, the player's world presence, and the remote actor
lifecycle: spawn, retirement, and respawn.

Actor engine mechanics belong to [Remote actor](remote-actor.md), each
feature's rules to its own system, the transport to [Network](network.md).

## Components

| Module | Responsibility |
| --- | --- |
| `runtime/PluginStartup` | Sizes the trampoline that `SKSE::Init` allocates, loads the configuration, installs every hook, and handles SKSE messages |
| `runtime/MainLoopHook` | The one main-loop detour; binds the game thread and runs a pass |
| `runtime/GameThread` | `Bind`, `IsAuthoritative`, `Require`: the engine-mutation thread guard |
| `runtime/GameSession` | Whether a game is loaded, set by `PresenceSync` |
| `runtime/VanillaIntro` | Whether the vanilla Helgen intro holds the connection ([Vanilla intro](#vanilla-intro)) |
| `runtime/RemoteActorActivation` | The local player's activation of a remote actor or mount, on its worker thread ([UI overlay](ui-overlay.md#player-menu-in-the-world)) |
| `runtime/PluginTime` | Plugin time per pass, averaged over 100 ms windows ([UI overlay](ui-overlay.md#plugin-time)) |
| `runtime/KeyState` | Keys and mouse buttons held as the input dispatch reports them, in one key-code space ([Configuration](configuration.md#controls)), for [voice chat](voice-chat.md#activation) |
| `runtime/PlayerGraphEventHook` | The local player's accepted graph input events, read once against the modded events, for [beast actions](combat-presentation.md#beast-actions), [idles](idle-presentation.md#capture), [pose windows](remote-actor.md#source-window), the [graph event survey](#graph-event-survey), and the [graph trace](#graph-trace) |
| `sync/SyncCoordinator` | One frame entry, the stage order, the single network drain, and the peer map |
| `sync/PeerState` | A peer's complete receive state, composed of feature-owned members |
| `sync/SyncFrame` | `LocalFrame`, `RemoteFrame`, `RemotePose`, and `CheckpointUpdate` views for one pass |
| `sync/CheckpointPlayout` | A peer's received checkpoints, its playback clock, the played checkpoint, and the interpolated pose |
| `sync/CheckpointPublisher` | Checkpoint cadence and sequence; calls every feature's local sampler |
| `sync/ActorLifecycle` | Spawn, retirement, stranded and lost actors, spawn throttling, save exclusion, the mount beside its rider |
| `sync/presence/PresenceSync` | `Enter` and `Exit`, and the game session |
| `sync/unsynced/UnsyncedCells` | The [unsynced locations](#unsynced-locations): the saved list, the local interior, and whether it is on the list |
| `sync/voicechat/VoiceChatSync` | [Voice chat](voice-chat.md)'s game-thread state: devices, the microphone key, speakers' parameters, occlusion, lip sync |
| `sync/customanimation/GraphEventSurvey` | Debug builds: logs each distinct graph event the local player accepts ([Graph event survey](#graph-event-survey)) |
| `sync/customanimation/GraphTrace` | Debug builds: logs the local player's modded graph behavior ([Graph trace](#graph-trace)) |

`sync/` holds features seen on the wire and their game-thread state; `actor/`
holds Skyrim actor mechanisms. For example, `CombatSync` decides when a network
attack may play, and `CombatIsolation` keeps the local world from fighting the
actor.

Dependencies run `runtime -> sync -> actor` for game-thread work,
`sync -> network/protocol` for data, `sync -> audio` for voice,
`actor -> protocol` for value types only, and `actor -> runtime/GameThread` for
the thread guard. `network/` and `protocol/` include no Skyrim headers.
`runtime` calls the installers in `actor`, which own their detours and event
sinks. `RemoteActor` (`actor/RemoteActor.h`) is the replicated actor instance;
`RE::Actor` stays qualified. Some log messages and `GameThread::Require` names
(`UpdateControlledNPC`, `SpawnControlledNPC`) keep older words so logs of
different builds stay comparable.

## Threading

- Skyrim engine reads and mutations run only on the thread of the main-loop
  hook. `GameThread::Bind` records the first thread that runs the hook, and
  every pass after must run on it; a migrated hook stops the pass.
  `RE::Main::threadID` does not identify the thread running the pass.
- `GameThread::Require` guards each public remote-actor operation that
  dereferences an engine object; a caller on another thread is refused and
  logged once.
- `PlayerCharacter::UpdateAnimation` and `SKSE::TaskInterface::AddTask` tasks
  run on changing worker threads, so neither may mutate an actor or graph.
  Writing `Actor::actorState1` movement bits from the animation hook crashed
  later in FaceGen, so the plugin never writes engine-owned movement-state
  fields.
- Network callbacks only validate and queue plain values behind a lock. Hook
  callbacks on worker threads only query `ActorRegistry`, or queue the local
  player's beast actions and idle events behind `ShapeshiftSync`'s or
  `IdleSync`'s own lock. Voice frames go from the network threads to the audio
  engine without the pass ([Voice chat](voice-chat.md#threads)).

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
   frame did not advance;
6. calls `VoiceChatSync::Maintain`, also when the frame did not advance, so
   devices follow the connection in the main menu and players talk with a
   menu open ([Voice chat](voice-chat.md#activation)).

The original call reaches Skyrim's per-frame actor movement jobs, so the engine
movement update runs before the pass and the network transform is the last
write before render ([Remote actor](remote-actor.md#frame-order)).

`SyncCoordinator::OnFrame`:

```text
NameplateSync::Clear                      # an early return leaves no plates
restore save eligibility after a save
DefaultLook::Refresh                    # re-resolve a changed [DefaultLook]
ApplyNetworkChanges                       # the one drain; trade events queued
ServePlayerDetails                        # the open player menu's level and place
PlayCheckpoints                           # each peer's due checkpoints play
ActorLifecycle::RefillSpawnAllowance
if no player: UnsyncedCells::UpdateLocal(nullptr), VoiceChatSync::UpdateLocal(nullptr, false), return
YieldActivatedFurniture                   # queued local furniture activations
OpenActivatedPlayerMenu                   # the latest activated remote actor's player menu
inWorld = PresenceSync::SyncPresence
VoiceChatSync::UpdateLocal(player, inWorld)   # heard only in the world; the player's lip sync
if inWorld:
    ShapeshiftSync::ObserveLocalForm       # the local beast form, for the graph input hook
    CombatSync::CaptureLocalAttack         # dropped in a beast form
    CombatSync::CaptureLocalHandPresentation
    VoiceSync::CaptureLocalVoice           # the shout's voice state
    AnimationSync::ObserveLocalGraphEpoch
    AnimationSync::ObserveLocalMovement    # stop hold timer
    GraphEventSurvey::LogLocalGraphEvents  # debug builds only
    EquipmentSync::SyncEquipment           # flushed when an attack, presentation, or shout started
    CombatSync::QueueLocalAttacks
    VoiceSync::QueueLocalPowers            # powers the spell cast event sink captured
    ShapeshiftSync::QueueLocalActions      # beast actions the graph input hook captured
    IdleSync::QueueLocalIdles              # idle events it captured; the held idle
    CheckpointPublisher::PublishLocalCheckpoint
        disconnected: discard queued attacks, beast actions, idle events, and powers;
                      reset hand and voice presentation to this pass's
        published: CombatSync::PublishLocalAttacks, ShapeshiftSync::PublishLocalActions,
                   IdleSync::PublishLocalIdles, then VoiceSync::PublishLocalPowers
    AppearanceSync::SyncAppearance
    EffectSync::SyncLocalEffects           # the presented active effects, polled
else: discard captured attacks, beast actions, and idle events
TradeSync::Sync                           # trade events, overlay commands, cancels, offers, a commit
unsynced = UnsyncedCells::UpdateLocal     # the player's cell, or null
if no local cell: return
PrecisionIsolation::Update                # reassert after the player's Havok world changes
for each peer: SyncPeer(unsynced)
VoiceChatSync::UpdatePeers(unsynced)          # speakers' gain, pan, occlusion; lip sync; speaking levels
NameplateSync::Update                     # head nodes, line of sight, the plates the overlay draws
RemoteLights::Update                      # with a world camera: the remote light budget
```

`SyncPeer` for one remote player:

```text
AppearanceSync advance timers; retire on a new appearance revision, or else on a new form
retire in an unsynced cell, or else on a coordinate-space change
CombatSync, ShapeshiftSync, IdleSync, VoiceSync::AgeRemoteActions
EffectSync::AgeRemoteEffects               # held effects take effect after the playout delay
not in the local space, or the cell is unsynced: clear attacks, beast actions, idles, and powers, stop
EffectSync::SyncEffects                    # with an actor, in every state below
DeathSync::SyncDeath                       # a killed actor keeps only handle and look; shout released
ActorLifecycle::RetireWhenStranded
FurnitureSync yield check                  # hide an actor that would block furniture
MountSync::SyncMount                       # riding: queued attacks, idles, and powers cleared,
                                           # shout released; equipment,
                                           # rider update with weapon state, look only
otherwise:
    MovementSync::SyncMovement             # played checkpoint's movement, then the pose
    EquipmentSync::SyncEquipment           # -> handsSettled, readyForWeaponDraw
                                           # a beast: SyncBeastEquipment, both true
    FurnitureSync::SyncFurniture           # exit or target change -> furniturePlaying
    AnimationSync::SyncAnimation           # unless an attack or furniture owns the graph
    FurnitureSync::SyncFurnitureEntry
    unless furniture owns the actor:
        a beast: ShapeshiftSync::SyncBeast # beast actions, levitation, casts, Mist Form and bats
        otherwise: IdleSync::SyncIdle      # release, then queued or held idles
                   CombatSync::SyncCombat, SyncHandPresentation
                   VoiceSync::SyncVoice    # the shout, then one queued power
    furniture or a beast: VoiceSync::ClearRemote
    AnimationSync::GetPresentableState     # none while furniture owns the actor
    ActorRuntime::Update
    AppearanceSync::ApplyActorPresentation
no actor: end a vacated furniture yield, ActorLifecycle::SpawnWhenDue
```

Cross-feature results (`handsSettled`, `readyForWeaponDraw`, `furniturePlaying`,
`attackPlaying`, a queued attack for `SyncIdle`, the presentable state) are
passed as explicit arguments. A
feature never reads another feature's state fields to infer them.
`ActorRuntime::Update` then applies the engine side in its own order
([Remote actor](remote-actor.md#update-order)).

## Local publishing

`CheckpointPublisher` publishes every 0.05 s of accumulated frame time while
connected, keeping the remainder so the average rate holds and dropping whole
missed intervals instead of bursting. A pass without a parent cell publishes
nothing. The sequence wraps before `INT64_MAX`. `Network::PublishLocalPlayerTransform` sends the sample's
context (cell, worldspace, graph epoch, furniture, shapeshift, idle) first whenever it differs
from the last one sent since `Enter`, then the quantized checkpoint
([Network](network.md#checkpoint-and-context)).

One checkpoint is one coherent sample: `MountSync::SampleLocalMount`,
`MovementSync::SampleLocalMovement` (from the mount while riding),
`AnimationSync::SampleLocalAnimation`, `FurnitureSync::SampleLocalFurniture`,
`CombatSync::SampleLocalHandPresentation`, `VoiceSync::SampleLocalVoice`,
`DeathSync::SampleLocalDeath`, `ShapeshiftSync::SampleLocalVampireLord`,
`IdleSync::SampleLocalIdle`, and `SkeletonSync::SampleLocalPose`
([Remote actor](remote-actor.md#skeleton-pose)), with the graph epoch and local
form observed this pass. No stage publishes a partial checkpoint. The
checkpoint precedes the `Action` anchored to it, and a weapon or shout swap is
published before the attack, presentation, or shout that uses it.
`PoseWindow::Update` runs before `IdleSync::QueueLocalIdles`, since an opening
window ends the held idle, and before the checkpoint, whose pose it gates.

Local sampling state lives in structs `SyncCoordinator` owns, not function
statics, so it can be reset with `= {}`: `CheckpointPublisher::State` (cadence,
sequence, `MovementSync::LocalState`) and `AnimationSync::LocalState` (graph
epoch, jump tracking, stop and arrival holds, last published state). They live
as long as the process.

Appearance, equipment, and effects publish on their own schedules and republish on every
new connection generation ([Network](network.md#latest-wins-publishers)).

### Graph event survey

A debug-build diagnostic (without `NDEBUG`, like the
[graph probe](locomotion.md#graph-probe)) showing which graph events a
modpack's animations use. Nothing acts on it.

- `GraphEventSurvey::ObserveLocalGraphEvent` records, on the hook's thread,
  each event name the player's graph accepts for the first time in the
  process, up to 1024 names.
- `GraphEventSurvey::LogLocalGraphEvents` logs `Local graph event first seen`
  for each one 0.3 s of clamped frame time later, so the transition it started
  has activated its clips, with the active graph (third or first person) and
  every active `hkbClipGenerator`'s animation from
  `GraphProbe::DescribeActiveClips`.

### Graph trace

A debug-build diagnostic showing how a modded graph behavior runs on the local
player: the events entering and leaving it, its states, and the variables it
reads and sets. Nothing acts on it.

- An event at index 1217 or later and a variable at index 230 or later of a
  graph with the vanilla prefix is modded
  (`GraphVocabulary::kVanillaEventCount`, `kVanillaVariableCount`,
  [Remote actor](remote-actor.md#source-window)). `GraphProbe::ReadTrace`
  traces only such a graph. Each graph change logs `Graph trace graph changed`
  with its modded event and variable names.
- `GraphTrace::ObserveLocalGraphEvent` queues every event the player's graph
  accepts (up to 256 between passes). `GraphTrace::Update` reads the active
  graph each pass and turns accepted events, state machine changes (current
  state, activation, deactivation), and changes of bool and integer variables
  into lines. Consecutive repeats of one event collapse into one line with the
  repeat count.
- A modded event or a modded variable change opens a 3 s window, extended by
  each later one; lines inside it are logged as `Graph trace:`. Lines from the
  1.5 s before it opens are kept (up to 128) and logged first with their
  offset in seconds, so the vanilla state the behavior started from shows.
  `Graph trace opened` and `Graph trace closed` bound each window.

## World presence

`PresenceSync` tells the server when the local player is in the world:

- In the world means no loading screen or main menu is open and the player has
  a parent cell and loaded 3D. The first such pass also starts the game session
  (`GameSession::SetInGame(true)`).
- On each connection generation the player starts out of the world, so
  `Enter` is sent again once it is in the world.
- `Exit` is sent once when a loading screen or the main menu opens (menu sink,
  before the main loop may stall on the load), when a save starts loading
  (`kPreLoadGame`), or when a pass finds the player out of the world. The main
  menu also ends the game session. The menu sink and SKSE message run outside
  the pass, so the published state is kept under a lock.
- Attacks captured while out of the world are discarded, so they cannot anchor
  to the first checkpoint after returning.

## Unsynced locations

Goal: a player can pick interiors where it sees no other players, such as a
home or a dungeon it wants to play alone, and keep playing there without
leaving the server. Other players are not told and still see the player
there. Non-goals: listing an exterior or a whole worldspace (exterior cells
meet at borders, so a player crossing one would pop in and out), hiding the
player from others (the server and peers are unchanged), listing by
location (`BGSLocation`) rather than by cell, saving bandwidth (the server
still sends the cell's peers), and a list per server.

- `UnsyncedCells` keeps the list of interior cells, each a
  [form reference](network.md#form-references) and the name it had when added,
  in `PlayerHub.locations` beside `PlayerHub.ini`, one line per cell:
  `Plugin.esp|0xFormID`, a tab, and the name. It is read on first use and
  written whole on each change (`PluginFiles::ReplaceFile`); a failed write
  posts a [notice](ui-overlay.md#notices) that the change lasts until the game
  exits. Malformed lines are logged and skipped, and a repeated cell is kept
  once. Plugin names compare without case. The list is the same on every
  server and needs no connection.
- `UpdateLocal` runs on the game thread once per pass. Only when the player's
  interior FormID or the list's revision changes does it build the cell's
  reference (`FormLookup::MakeFormReference`) and name (`GetFullName`, else
  the editor ID, else the reference), look the cell up, and publish the
  result under the module's lock for the overlay (`GetLocalCell`). An
  exterior, no cell, or an interior without a plugin (a dynamic form)
  publishes none, which cannot be added. A change between hidden and shown
  logs `Unsynced cells hide remote players in ...` or
  `Unsynced cells show remote players again` at info.
- `AddLocalCell` and `Remove` may run on any thread: the overlay adds the cell
  `UpdateLocal` published, so it reads no engine state. Each change bumps the
  revision, so the next pass applies it.
- While the player's cell is on the list, every peer counts as outside the local
  space: its actor and mount are retired (`RetireForUnsyncedCell`, logged as
  `Recycling remote player actor ... in an unsynced cell`), queued actions are
  cleared, and nothing spawns. The reconstructable state is kept, so the
  players [spawn](#actor-lifecycle) again once the player leaves the cell or
  the cell is removed from the list. [Voice chat](voice-chat.md#playback)
  hears nobody there, and without actors there are no nameplates or world
  player menus, and no trade can be offered to a player there, since the
  menu's trade item needs its actor in range. Chat and the player list are
  unchanged.
- The list is edited in the settings window's `[Gameplay]` page
  ([UI overlay](ui-overlay.md#unsynced-locations)).

## Vanilla intro

`VanillaIntro` tells the [connection](network.md#vanilla-intro-hold) and the
overlay whether the vanilla prisoner intro is playing, from quest MQ101
(`Skyrim.esm` `0x0003372B`) and the prisoner cuffs `PrisonerCuffsPlayer`
(`Skyrim.esm` `0x0010E039`), looked up at `kDataLoaded`; without either form
the intro never holds. `IsPlaying` reads an atomic, so the overlay may call it;
`Update` sets it on the main loop.

- It holds while MQ101 is running (`TESQuest::IsEnabled`; CommonLib's
  `IsRunning` is also true for a stopped quest) below stage 160
  (`kPlayerControlStage`, the first free play after the cart) and the player is
  the prisoner.
- The player is the prisoner once `Actor::GetWornArmor` finds the cuffs worn
  below stage 160, kept for the rest of the check (`s_prisoner`). MQ101 running
  is not enough: Alternate Perspective keeps it at stage 0 through its own
  start and runs its Helgen visitor path with the player free, neither
  equipping the cuffs, while the vanilla start and that mod's cart branch do.
- A stopped, completed, or never started MQ101 never holds, so an alternate
  start, a console skip, or a later stage connects at once.
- The quest is checked only while the hold can change: from `kNewGame`, or from
  `kPostLoadGame`, each pass until the hold is off, then not until the next
  game (`s_restart`), so a cart ride begun later is not held. Between
  `kPreLoadGame` and `kPostLoadGame` the hold keeps its last value, since the
  quest state is the previous game's or partly loaded.
- At `kNewGame` SKSE passes the start quest. When it is MQ101 the hold starts
  at once (`s_awaitingStart`), since stage 10 equips the cuffs some Papyrus
  time after the player reaches the world. The wait ends when the cuffs are
  worn, MQ101 completes or reaches stage 160, at `kPreLoadGame`, when the race
  menu (`RE::RaceSexMenu`) closes without the cuffs (`s_raceMenuSeen`), or
  after 5 s (`kStartWaitSeconds`) of clamped frame time without cuffs or race
  menu; the last two log a prisoner start that did not happen. The open race
  menu holds the wait regardless of time: the prisoner start opens it long
  after stage 10, while Alternate Perspective's opens it before any stage, so
  that start connects when it closes. Other start quests start no wait.
- `IsPlaying` is false outside a game (`GameSession::IsInGame`).
- Each check ending with the hold off, and each change, logs `Vanilla intro
  hold on` or `off` with MQ101's running flag and stage, the prisoner flag, and
  the wait; `kNewGame` logs whether its quest is MQ101.

## Receive state

`SyncCoordinator` owns one `std::unordered_map<Multiplayer::PeerSlot, PeerState>`
keyed by the peer's server slot. `PeerState` holds the peer's
`CheckpointPlayout`, the sequence the current actor last received, and the
feature-owned `combat`, `shapeshift`, `idle`, `lifecycle`, `appearance`,
`equipment`, `effects`, `furniture`, and `nameplate` members; movement and
animation keep theirs on the `RemoteActor`. A `RemoteFrame` presents the played checkpoint,
its sequence, and the interpolated pose; features never see a checkpoint that
has not played.

`ApplyNetworkChanges` drains everything the network thread queued, once per
pass:

1. A lost or replaced connection removes every peer, since slots belong to one
   connection; reconnecting rebuilds them from `PeerEnter`.
2. A `PeerLeave` for any reason but `DISCONNECTED` (the peer left the world,
   went out of range, or was excluded) removes the actor and resets the
   playout and combat state, but keeps appearance, equipment, and effects
   until a `PeerEnter` places the player again. Leaves apply first, since
   changes drained with them arrived after them.
3. `PeerLeave` with `DISCONNECTED` removes the actor and all state.
4. Checkpoints, each already merged with the peer's context and stamped with
   its arrival time on the transport thread, go into the peer's playout with a
   rising sequence. A stale one is logged and ignored. A `PeerEnter` delivers
   its stored checkpoint the same way.
5. Appearance, equipment, and effects are latest-wins by revision and may
   arrive before any checkpoint. None alone spawns an actor.
6. Actions are ordered and bounded by the combat, shapeshift, or idle system,
   by kind, and accepted against the newest received checkpoint, played or not.
7. Inspections go to `PlayerDetails::Accept`
   ([UI overlay](ui-overlay.md#player-menu)).
8. Trade events go to `TradeSync::QueueNetworkChanges`, which ends a trade
   whose connection was lost or whose events were dropped and keeps the rest
   for `TradeSync::Sync` ([Trade](trade.md)).

`PlayCheckpoints` then advances every peer's playout. Each checkpoint that
reaches its playback time is shown, in order, to furniture, animation,
combat, shapeshift, idle, and voice (`OnCheckpointPlayed`) before it replaces
the played one. Furniture
resolves its reference into the local load order there, once per change.

Furniture occupancy, weapon stance, the held idle, and every other checkpoint
field are reconstructable; historical attacks and ended idles are not.

## Checkpoint playout

Checkpoints leave the source every 0.05 s but arrive unevenly: the server
batches them per send interval, replaces overtaken ones, sends far peers at
`far_rate_hz`, and suppresses idle repeats
([Interest and delivery](interest-and-delivery.md#batching)), and the network
adds jitter. Following each newest checkpoint would make the actor alternate
between catching up and waiting, so `CheckpointPlayout` plays each peer's
checkpoints at their sampled spacing, a little behind the newest. In a
simulation (60 fps source, 20 Hz relay with idle suppression and jitter spikes,
receivers at 30–144 fps) this cut a following actor's per-pass speed deviation
from about 23% to 7%, and passes without movement from 3% to 0.3%, for about
50 ms more delay. The whole checkpoint waits, so movement state, jumps, stance,
hands, furniture, death, and anchored actions stay with the position they were
sampled at ([Combat presentation](combat-presentation.md#receiver-queue)).

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
- **Pose.** Between the played checkpoint and the next waiting one, position,
  pitch, and look pitch interpolate linearly and heading along the shorter arc.
  `RemotePose.speed` and `headingRate` are the segment's distance and turn over
  its duration (0 without a segment), `fraction` the progress through it, and
  `RemoteFrame.next` (`CheckpointPlayout::GetNext`) the checkpoint it moves
  toward, for the [skeleton pose](remote-actor.md#skeleton-pose).
  `sampledSpeed` interpolates the checkpoints' `speed`, and `direction` their
  `direction` along the shorter way while both move and differ by less than
  1/8 turn; otherwise it holds the played one, so a larger change steps and is
  cross-blended ([Locomotion](locomotion.md#direction)).
- A gap over 0.25 s means the player stood still (idle repeats are not sent) or
  the connection stalled, so the segment starts only the next checkpoint's
  distance over its speed before it, at least one `interval`, at most the gap.
  Nothing interpolates across a coordinate-space or mount change or more than
  4096 units. With nothing waiting the pose holds the played checkpoint; there
  is no extrapolation. A wait of 0.02 s or more that began while moving at 50
  or more logs its end (`Checkpoint playout ran dry`, with duration and
  estimates; the first 10, then every 100th).

Every step is constant work per peer per pass with no allocation after a
peer's first checkpoint.

## Coordinate spaces

A checkpoint's context names the player's space with a
[form reference](network.md#form-references): `interior_cell` in an interior,
`worldspace` outside ([Network](network.md#checkpoint-and-context)), so load
orders need not match. Exterior checkpoints with the same worldspace share the
exterior grid's coordinates and stay in sync across cell boundaries. An
interior needs an exact cell match, because its coordinates are local to it.

- **Publishing.** `MovementSync::SampleLocalSpace` builds the reference once
  per space change; a space without a form reference publishes neither and
  logs a warning. It also builds the `location` reference once per change of
  `PlayerCharacter` `currentLocation` (none for a dynamic location), which only
  names the place in the [player menu](ui-overlay.md#player-menu).
- **Resolution.** `MovementSync::OnCheckpointPlayed` resolves a peer's
  reference with `FormLookup::ResolveFormID` on its first played checkpoint
  and on each change, into `PeerState::space`, which `RemoteFrame::cellID`
  and `worldspaceID` carry. `MovementSync::IsInLocalSpace` compares those local
  FormIDs with the player's own. A reference whose plugin is not loaded
  resolves to zero, matches no local space, and is logged once per change: the
  peer is not spawned.
- **Comparisons.** Playout interpolation compares two checkpoints' references
  themselves (`MovementSync::IsSameCoordinateSpace`). A spawned actor keeps
  the resolved IDs of its space (`ActorLifecycleState::spawnedCellID`,
  `spawnedWorldspaceID`) and is retired when they change.

## Actor lifecycle

`ActorLifecycle` creates and retires the peer's `RemoteActor` and, while the
player rides, its mount ([Native interactions](native-interactions.md#mounts)).

- **Spawn.** An actor is created at the played checkpoint's position and
  heading, never at the local player first, once the peer is in the local
  space, the location is loaded (`ActorSpawner::IsSpawnLocationLoaded`: an
  exterior cell containing it, or the local player's interior), and
  `AppearanceSync::GetSpawnBase` has a base for the checkpoint's form. Right
  before `ActorSpawner::Spawn`, `AppearanceSync::OnActorSpawning` gives the
  base its [face sliders](appearance-and-equipment.md#face-sliders). A failed
  spawn retries after 1 s. A spawn records its cell and worldspace.
- **Throttling.** `[Advanced] SpawnsPerSecond` (default 20, `0` unlimited)
  bounds actor and mount spawns. `RefillSpawnAllowance` adds the rate times the
  frame time once per pass, never saving up more than one pass's share, and a
  pass may always start one spawn. A held-back spawn accumulates its wait, and
  a due spawn starts only once it has waited as long as the longest spawn held
  back in the previous pass, so a fixed peer order cannot starve later peers.
- **Retirement** removes the actor and its mount, clears queued attacks, idles,
  and the applied sequence, and keeps the reconstructable state, the held idle
  included, for a respawn. Queued beast actions stay, since they may belong to
  the form the actor respawns in, and age out after 3 s. Reasons, each logged
  as `Recycling remote player actor ...`: a new appearance revision, a new
  [beast form](appearance-and-equipment.md#shapeshifts), a coordinate-space
  change, an [unsynced cell](#unsynced-locations), fast travel, revival,
  dismount (including a change between a horse and a dragon), a lost mount, and
  death while riding.
- **Fast travel** within one space keeps the coordinate space, so it is found by
  displacement: an actor more than 4096 units (`kRelocationDistance`) from its
  checkpoint is retired, and so is one without 3D for 2 s whose target location
  is not loaded, since its own cell stayed unloaded and it cannot regain 3D.
- **A lost handle** schedules a respawn after the retry delay; a `PeerLeave` or
  lost connection removes actor and mount without one.
- **Removing a reference** releases its furniture occupancy and marker
  reservations first (an orphaned reservation makes Skyrim refuse every entry
  point to the replacement), unregisters it, then disables and deletes it.

### Saves

Remote actors belong to the session, not the save. On `kSaveGame`, dispatched
before the engine writes, every live actor's reference is excluded from the
save; the next pass restores it. A removed reference is excluded permanently:
deletion does not retire its change form, which carries the never-serialized
runtime base and on load would bind to whichever dynamic form got the same ID,
crashing the load.

## Timing

Durations accumulate the frame delta clamped to 0.1 s (`FrameTime::Clamp`);
pass counters remain only for engine update passes or log throttling
(`ActorConstants::ShouldLogOccurrence`). Two clocks are exceptions. The checkpoint playout follows the sender's clock,
so it runs on `steady_clock` and catches up after a local pause instead of
replaying it. `CheckpointPublisher` accumulates the unclamped delta, because
the elapsed time divides the distance travelled into the published speed.

## Engine assumptions

- [Vanilla intro](#vanilla-intro) stage meanings come from the 1.7.104 data
  files: `QF_MQ101_0003372B` Fragment_316 (stage 160) restores control and
  movement; Fragment_4 (stage 10) and Fragment_227 (stage 70) equip
  `PrisonerCuffsPlayer`, Fragment_74 (stage 240) removes it; the prisoner start
  opens the race menu only from Fragment_13 in the Helgen yard. Alternate
  Perspective's `QF_MQ101_0003372B_new` equips the cuffs only in its cart branch
  (stage 4, then Fragment_389), not in its start (Fragment_332) or visitor path,
  and `APMQ101` opens the race menu at its start.
- `GetWornArmor` sees a Papyrus `EquipItem` by the next pass.
- SKSE dispatches `kNewGame` after `TESQuest::EnsureQuestStarted` (ID 25003)
  returns, with that quest as `data` (read from `skse64_1_7_104.dll`; assumed
  for SE; the pointer is only compared). The new-game wait guards against the
  player reaching the world before a queued MQ101 start runs, which was never
  observed.
- `kPostLoadGame` arrives before the loaded game's first pass with the player
  in the world, so a save in the intro starts with the hold set.

## Acceptance criteria

- In an interior on the [unsynced locations](#unsynced-locations) list, no
  remote player or mount is shown and no remote voice is heard; players
  already there disappear in the pass after the cell is added.
- Leaving the cell, or removing it from the list, shows the players there
  again without reconnecting.
- Players in the cell still see the local player, and the list is kept
  across games and servers.

## Known limitations

- Once the [vanilla intro](#vanilla-intro) hold is off, nothing reads MQ101
  until the next load or new game, so setting it back below stage 160 from
  the console does not hold the connection again.
- A save made during a cart ride begun later in the play-through (Alternate
  Perspective's cart branch) holds the connection when loaded, because a
  save does not record how the ride began.
- A vanilla start whose stage 10 fragment runs more than 5 s of game time
  after the player reaches the world, as with a heavily loaded Papyrus VM,
  is not held.
- Interior and worldspace changes despawn and respawn the actor; there is no
  presentation through loading screens.
- Remote players are shown the playout delay behind their network latency,
  typically 0.1–0.2 s in total. There is no extrapolation or latency
  compensation; when nothing waits to play, the actor stops at the played
  checkpoint and drains the gap once the next one arrives.
- The playout constants were tuned in simulation, not under real jitter or a
  far peer's 10 Hz rate.
- There is no lobby or server-authoritative movement.
- A player in an [unsynced cell](#unsynced-locations) can still be invited
  to a trade by a player there, who sees it, and the prompt shows as usual.
