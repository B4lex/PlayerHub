# Performance

## Status

The [measurements](#measurements) ran under Proton, not on native Windows,
where socket calls cost less, so the checkpoint send is likely cheaper there.

## Scope

How the plugin spends each frame, how that cost grows with remote players,
what is already bounded or throttled, and how to measure it. The mechanisms
themselves belong to their systems; this spec names them and links there.
Server throughput and its measurements are in
[Interest and delivery](interest-and-delivery.md#measurements).

## Components

| Module | Responsibility |
| --- | --- |
| `runtime/Profiling.h`, `runtime/Profiling.cpp` | Tracy macros: zones, plots, the frame mark, opt-in lock wrappers, hook counters, allocation counters; all expand to nothing without `TRACY_ENABLE` |
| `runtime/ProfilingMemory.cpp` | In Tracy builds, replaces the plugin's global `operator new` and `delete` to count allocations, and with `tracy_memory` reports them to Tracy |
| `runtime/PluginTime` | The always-built plugin time the status panel shows ([UI overlay](ui-overlay.md#plugin-time)) |
| `xmake.lua` options `tracy`, `tracy_locks`, `tracy_memory` | Tracy build switches |

## Where the time goes

The plugin's own frame cost is **plugin time**: the main-loop hook's pass
([Sync pipeline](sync-pipeline.md#frame-sequence)), the overlay drawn from
the renderer's end hook ([UI overlay](ui-overlay.md)), and input routing.
`PluginTime` sums these, and in the captures all three ran on the same thread,
so plugin time adds to the frame directly.

Work elsewhere does not count toward plugin time:

- **Engine hooks** run inside Skyrim's own per-actor work on its animation, AI,
  and actor update threads, for every actor in the loaded area, not only remote
  ones: graph channel polls, graph input, pose copy, movement isolation, the
  actor update policy, hostility checks, aim targets, and the speed sampler
  ([Remote actor](remote-actor.md#graph-channels)). Each call is a registry
  lookup and a small write.
- **The connection thread** decodes and validates server messages
  ([Network](network.md#client)).
- **Voice** runs on its device and datagram threads
  ([Voice chat](voice-chat.md#threads)).
- **Skyrim itself** updates, animates, and draws each spawned remote actor as it
  would an NPC. Past a few dozen actors this is the larger part of what remote
  players cost a frame.

## Measurements

Taken on one Linux machine running Skyrim AE 1.7.104 under Proton, with 200
bots replayed from one recording
([Recording and replay](recording-and-replay.md#load-testing)). Hardware,
graphics mods, and the frame limit were not recorded, so frame times compare
runs with each other, not with other machines.

### Growth with remote actors

Medians per main-loop pass while 200 bots spawned at the default
`[Advanced] SpawnsPerSecond` (20) and then stayed:

| Spawned remote actors | Main loop | Overlay | Plugin time | Frame time |
| --- | --- | --- | --- | --- |
| 0 | 25 µs | 43 µs | 67 µs | 12.1 ms |
| 0–24 | 290 µs | 67 µs | 357 µs | 12.1 ms |
| 50–74 | 744 µs | 93 µs | 837 µs | 12.1 ms |
| 100–124 | 1.38 ms | 107 µs | 1.49 ms | 14.3 ms |
| 150–174 | 1.94 ms | 101 µs | 2.04 ms | 16.8 ms |
| 200, settled | 2.03 ms | 98 µs | 2.13 ms | 16.2 ms |

- The main loop grows linearly, by about 10 µs per spawned actor. Rows taken
  while actors still spawn also carry spawns (about 240 µs each).
- The overlay grows with nameplates and levels off near 100 µs, since only
  plates in sight are drawn.
- Frame time held at 12.1 ms up to about 75 actors, so up to there something
  other than the CPU bounded the frame. Beyond that it grew by about 4 ms at 200
  actors, of which plugin time was about 2 ms and Skyrim's own work on the
  actors the rest.
- At the default interest cap of 64 visible peers
  ([Interest and delivery](interest-and-delivery.md#subscriptions)) plugin time
  is under 1 ms.

### Per actor and pass

At 200 settled actors, in nanoseconds per spawned actor and pass (nested
stages are included in their parent):

| Stage | ns | Note |
| --- | --- | --- |
| `SyncPeer` | 9,300 | its own code 1,200 |
| `ActorRuntime::Update` | 6,300 | |
| `ActorAnimation::ApplyAnimationUpdate` | 2,500 | |
| `ActorMovement::ApplyTranslation` | 1,800 | `SetPosition`, heading, ground offset |
| graph variable writes | 1,100 | ten `SetGraphVariable*` per actor |
| `SendNetworkAnimationEvent` | 900 | about 4 events per pass, 22 µs each inside Skyrim |
| `GraphRecovery::Recover` | 530 | |
| `CombatSync::SyncHandPresentation` | 490 | |
| `EquipmentSync::SyncEquipment` | 480 | |
| `SurvivalPolicy::ApplyRemoteReplicaPolicy` | 430 | |
| `GraphStance::ReadStanceSelectors` | 300 | 4.5 µs per read |
| `PlayCheckpoints` | 280 | |
| `SurvivalPolicy::SanitizeRemoteCombatState` | 280 | |
| `SkeletonSync::SyncSkeleton` | 180 | |
| `AppearanceSync::ApplyActorPresentation` | 140 | |

No single stage dominates: the cost is many small steps, each paid for every
actor on every pass.

### With no remote players

About 70–90 µs of plugin time at 82 fps:

- The overlay, 45 µs while something is on screen: `Overlay new frame`
  (DX11 backend, fonts, input, and `NewFrame`) 19 µs, `Overlay windows`
  19 µs, `Overlay render` 6 µs. With nothing on screen the render is skipped
  and windows draw nothing, so the overlay takes 26–35 µs, of which
  `Overlay new frame` is still 17–21 µs. The ImGui frame is built every frame,
  whether or not anything is shown.
- The main loop, 25–45 µs. Most of it is the checkpoint send: about 20
  checkpoints a second, each written to the socket on the main thread through
  IXWebSocket's `sendBinary`, 60–80 µs per write under Wine against 4 µs to
  encode, about 15 µs per frame on average. `PoseWindow::Update` is 4 µs.

### Other threads

- Engine hooks at 200 actors: about 0.4 ms of CPU per pass across all threads,
  0.1–0.3 µs per call. The largest are pose copy (about 590 calls per pass),
  the Direction and TurnDelta channels (430 each), and the weapon speed
  channels (480).
- The connection thread: about 1.5 messages per pass at 200 actors, 40–50 µs
  each, about 30 allocations per pass.
- A spawn (`ActorSpawner::Spawn`): about 240 µs on the main loop, at most
  `SpawnsPerSecond` a second.

### Allocations

Counted with the Tracy build's allocator: about 1.5 per pass with no peers, and
about 13 per pass on the main-loop thread at 200 actors.

## What affects frame time

Affects it:

- **The number of spawned remote actors.** Plugin time grows by about 10 µs
  per actor per frame, and Skyrim's own cost per actor is larger. This is the
  dominant factor; the interest cap bounds it.
- **Spawns** cost about 240 µs each; `SpawnsPerSecond` spreads them out.
- **Graph events** sent to remote actors cost about 22 µs each inside Skyrim;
  most passes send a few.
- **Equipment reconciliation passes** add, remove, and equip items. They run
  only after an equipment change, a spawn, or a mismatch, but actors that
  spawn together reconcile in the same passes: while 200 bots spawned, single
  passes spent up to 7 ms in `EquipmentSync::SyncEquipment`.
- **The overlay** costs a fixed 40–100 µs whenever the plugin is loaded, more
  with open windows and nameplates.

Does not measurably affect it:

- Registry lookups in engine hooks: there is no lock contention in a normal
  build (0.1–0.3 µs per hook call).
- Receiving and decoding: it runs on the connection thread.
- Allocations ([Allocations](#allocations)).
- Stance selector reads, nameplate sight checks, and the remote light budget:
  each under 0.1 ms per pass at 200 actors.

## Optimizations in place

- **Interest.** The server sends each client at most `interest.max_visible`
  (64) peers and the poses of the nearest `interest.max_posed` (32), far peers
  at `interest.far_rate_hz` (10), suppresses repeats from idle peers, and lets
  at most `budget.enters_per_second` (20) peers enter per second
  ([Interest and delivery](interest-and-delivery.md)).
- **Spawn rate.** `[Advanced] SpawnsPerSecond` (20) bounds spawns per second
  ([Sync pipeline](sync-pipeline.md#actor-lifecycle)).
- **Registry fast paths.** Hooks that act only on gait-driven, flying,
  network-placed, or attack-scaled actors return before the registry lock
  while no actor has that flag; the main thread reads the registry without the
  lock ([Remote actor](remote-actor.md#registry)).
- **Stance sampling.** Selector states are read every
  `kGraphStanceSampleSeconds` (0.1 s) and when an edge needs them, through an
  exact vtable match instead of an RTTI cast per behavior node
  ([Locomotion](locomotion.md#stance-reconciliation)).
- **Ground probes.** An actor is probed again after its source moves, at most
  every `kGroundProbeIntervalSeconds` (0.1 s), and otherwise every
  `kGroundProbeRefreshSeconds` (1 s); a pass casts at most
  `kMaximumGroundRaysPerPass` (24) rays
  ([Locomotion](locomotion.md#ground-offset)).
- **Equipment checks.** A mismatch is checked every 0.25 s; a matched actor or
  one whose attempts are spent is compared without allocating and read in full
  every 5 s ([Appearance and equipment](appearance-and-equipment.md#reconciliation)).
- **Nameplate sight checks.** A plate is checked every `kSightCheckSeconds`
  (0.2 s), or `kOffscreenCheckSeconds` (0.05 s) while off screen, where no ray
  is cast; a pass casts at most `kMaximumSightChecksPerPass` (8) rays
  ([UI overlay](ui-overlay.md#nameplates)).
- **Remote lights.** `[Advanced] LightsLimit` (0 by default) caps the remote
  lights that light the scene ([Remote actor](remote-actor.md#light-budget)).
- **Item preview.** A spinning model is captured at most every
  `kSpinCaptureIntervalSeconds` (1/30 s) ([UI overlay](ui-overlay.md#3d-preview)).
- **Connection thread.** The client sends no WebSocket pings, so IXWebSocket's
  thread sleeps until data arrives or a send wakes it
  ([Network](network.md#transport)).
- **Empty overlay frames.** The overlay skips the render when the frame draws
  nothing ([UI overlay](ui-overlay.md#rendering)).
- **Load-time work off the main loop.** The item catalog and the localization
  catalogs are built on worker threads at startup.

## Profiling

- Configure `xmake f -m releasedbg --tracy=y`, the optimized mode with symbols,
  and connect Tracy 0.14.x. A debug build measures unoptimized code and the
  debug-only diagnostics.
- `--tracy_locks=y` records the locks declared with `PROFILE_LOCKABLE` and
  `PROFILE_SHARED_LOCKABLE` (the actor registry's), and `--tracy_memory=y`
  reports every plugin allocation to Tracy. Both are off by default: Tracy
  sends lock and memory events through one global mutex
  (`tracy::Profiler::m_serialLock`), and at a few thousand events per frame
  that mutex serialized the animation, AI, and main-loop threads and inflated
  every timing in the capture (hook calls read 2.5 µs instead of 0.15 µs, the
  main loop 5.4 ms instead of 2 ms). Use them only for the lock or memory view,
  not for timings.
- Zones mark each sync stage and per-peer function (`PROFILE_FUNCTION`,
  `PROFILE_SCOPE`), never each call of a frequent engine hook.
- Engine hooks keep totals instead (`PROFILE_HOOK_COUNTER`, `PROFILE_HOOK`,
  `PROFILE_HOOK_RESUME`): each hook's call count and own time, without the
  engine function it wraps, plotted once per pass as `Hook <name> calls` and
  `Hook <name> us`.
- Allocations are counted in every Tracy build: `Plugin allocations` and
  `Plugin allocated bytes` over all plugin threads, `Main-loop thread
  allocations`, and `Allocations <stage>` for the calling thread within a
  stage (`PROFILE_ALLOCATIONS`), nested stages included.
- Other plots: `Remote peers`, `Remote actors`, `Nameplates`,
  `Inbound checkpoints`, `Inbound actions`, the inbound appearance, equipment,
  and effects count, and `Server message bytes`.
- `tracy-csvexport` reads a capture on Linux: `-e` gives self times, `-u` every
  zone, and `-u -p` the plots too.
- A load test that resembles play: record a player in one spot, then run a
  batch of bots from the recording there
  ([Recording and replay](recording-and-replay.md#load-testing)); bots replay
  the recorded positions, so the local player stands among them.

## Engine assumptions

- The renderer's end hook ran on the main-loop thread in every capture, so the
  overlay's cost adds to the main loop's in the same frame.
- Skyrim runs character updates and graph updates on job threads, so engine
  hook cost spreads over cores rather than adding to the main loop.

## Acceptance criteria

1. With no remote players, plugin time stays below 0.1 ms.
2. At the default interest cap (64 visible peers), plugin time stays below 1 ms.
3. Plugin time grows linearly with spawned remote actors; no pass does work
   proportional to the square of the actor count.
4. In steady state the main-loop thread allocates fewer than 20 times per pass
   at 200 actors.
5. A build without `--tracy=y` contains none of the profiling code.

## Known limitations

- No level of detail per actor: every spawned actor gets every step on every
  pass regardless of distance or visibility. Throttling far or hidden actors is
  the lever for more than 64 visible players; it was not done because the
  default cap keeps plugin time under 1 ms.
- The overlay builds an ImGui frame even when nothing is shown; only the
  render is skipped. Skipping the frame needs every window to report whether
  it would draw.
- The checkpoint send writes to the socket on the main loop (about 15 µs per
  frame under Wine). Sending from a thread of its own removed it in a test
  build and is deferred until a release can cover its reconnect and exit
  paths.
- `RemoteLights::Update` walks every active scene light each pass (about
  60 µs).
- `WriteVariables` writes ten graph variables per actor per pass even when
  unchanged; skipping is safe only for variables no graph node or channel
  rewrites.
