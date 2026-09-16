# Network Synchronization POC

## Status

Implemented as an exterior-worldspace transform synchronization proof of
concept. Every plugin publishes its local player transform to the FastAPI
Socket.IO server at 20 Hz. The server validates and relays snapshots to all
other clients. Each remote Socket.IO connection owns at most one controlled NPC
puppet on the game thread.

## Runtime architecture

```text
Skyrim main-loop call hook (one bound owner thread)
    -> publish local position, heading, measured speed, movement/jump state, cell/worldspace IDs, sequence
    -> drain queued remote snapshots
    -> create/update/remove ID-keyed controlled NPC puppets

Socket.IO callback (network thread)
    -> validate and parse plain message data
    -> replace the pending transform/locomotion/jump checkpoint for that remote player ID
    -> never access RE/SKSE game objects

FastAPI + python-socketio server
    -> validate player:update
    -> reject stale sequence numbers
    -> relay player:state to other clients
    -> relay player:left on disconnect
```

## Protocol

The client emits `player:update`:

```json
{
  "x": 0.0,
  "y": 0.0,
  "z": 0.0,
  "heading": 0.0,
  "speed": 400.0,
  "direction": 0.0,
  "movementState": "running",
  "jumping": false,
  "cellId": 16513,
  "worldspaceId": 60,
  "sequence": 1
}
```

The server adds its Socket.IO session ID as `playerId` and emits the resulting
payload as `player:state` to every other connection. On disconnect it emits
`player:left` with the same `playerId`.

Only the newest pending transform/locomotion/jump checkpoint per remote player
is retained between game frames. Movement and jump phase are current state, not
discrete action history: replaying skipped movement or jump events would make
the graph traverse obsolete states while the puppet already follows the newest
target. The receiver derives transient takeoff and landing events only when its
applied checkpoint changes jump phase.

## Game-thread behavior

- Network callbacks only write plain C++ values behind a mutex.
- The Skyrim main-loop hook drains those values and performs all actor and
  animation-graph work on its single bound owner thread.
- Exterior snapshots with a matching non-zero worldspace form ID synchronize
  across cell boundaries. Their world coordinates are shared by the exterior
  grid.
- Interior snapshots use a zero worldspace ID and require an exact cell-ID
  match, because their coordinates are local to that interior.
- When the remote snapshot and local player stop sharing a coordinate space,
  the old reference is retired so it cannot remain visibly stranded in the
  previous interior or worldspace. The latest network state is retained, and a
  new reference is created at the exact remote snapshot once both players share
  a space again. Exterior cell changes within one worldspace remain continuous.
- A new puppet reference is created directly at its first received snapshot;
  there is no intermediate spawn at the local player's position.
- Puppets use a runtime actor base built from the source player's synchronized
  appearance (`specs/006-player-appearance-sync.md`), falling back to the local
  player's actor base until one is available. Equipped items are synchronized
  separately (`specs/007-player-equipment-sync.md`). Both remain separate from
  animation ownership and combat suppression.
- Puppet references retain normal actor, graph, controller, and collision
  processing. Attack/cast decisions are disabled, and immediately before each
  actor update the live AI process ignores combat and any combat controller is
  inactive. A puppet-only graph hook rejects locally injected events; the
  network controller sends replicated locomotion directly to the saved graph
  function.
  During an authoritative landing, a graph output-event sink observes the
  delayed `JumpLandEnd` annotation so the landing branch can return to
  locomotion. The landing edge is not complete until this marker is observed
  (with a bounded fallback), after which the controller reasserts grounded
  locomotion so the puppet cannot exit the branch in a sliding idle pose.
  Transform interpolation remains live while the timed landing branch finishes.
  This prevents local combat responses from taking graph ownership away from
  replicated locomotion without producing a T-pose or non-colliding puppet.
- Hit callbacks only enqueue recovery data. The game-thread update uses
  Skyrim's native animation-graph revert, waits for graph/controller stability,
  and then replays the newest locomotion checkpoint. A 3D rebuild is retained
  as a fallback when the native revert is rejected or does not become ready.
- The server's explicit `player:left` event removes the puppet and its receive
  state. A coordinate-space mismatch retires only the actor reference while
  retaining the latest receive state for an exact-cell respawn. A local
  Socket.IO transport failure retains all puppets during reconnection.
- Sequence numbers are checked by both server and client to ignore stale state.
  Network-thread coalescing also retains the highest pending sequence, so a late
  older packet cannot replace a newer packet before the game thread drains it.
- Position approaches the latest authoritative target at the source player's
  measured snapshot-to-snapshot speed; while airborne it also uses a bounded
  positional-error catch-up rate so a standing jump cannot stall when sampled
  speed is zero. Heading is applied from the remote snapshot.
- Heading is the source actor's body angle, so a backpedalling or strafing
  player keeps facing the same way. `direction` is the travel direction relative
  to that heading, in the behavior graph's `Direction` units in `[0, 1]`
  (0 forward, 0.25 right, 0.5 backward, 0.75 left). It is read from the source
  graph's `Direction` variable. When a custom graph does not expose it, it is
  derived from horizontal snapshot-to-snapshot travel. The puppet writes it to
  its own `Direction` variable while moving. Vanilla locomotion selects
  forward/backward/strafe clips only through `Direction`
  (`BSCyclicBlendTransitionGenerator.fBlendParameter` over `*_Direction_Blend`);
  there is no separate backward movement event. Skyrim also copies each actor's
  own movement direction into that variable through `ActorDirectionChannel`
  during every animation update, after main-loop graph writes; for a puppet
  that is always forward, so the channel's polled value is replaced with the
  network direction. Like Skyrim does for the player, a direction change of at
  least 1/8 of a turn is cross-blended with `CyclicFreeze` followed one graph
  update later by `CyclicCrossBlend`, at most once per 0.25 seconds so the
  0.2-second blend is not restarted while the source's `Direction` ramps.
- Animation state is selected by the source player and replicated explicitly as
  `idle`, `walking`, `running`, `sprinting`, `sneakingIdle`,
  `sneakingWalking`, or `sneakingRunning`, plus an orthogonal `jumping` flag.
  The graph's `bInJumpState` provides the responsive takeoff edge. After the
  player has actually become airborne, physical ground contact provides the
  landing edge because the graph flag can remain set during landing cleanup.
- Puppet translation uses the replicated numeric speed independently of the
  replicated animation state. Neither distance nor numeric speed selects the
  walking versus running animation.
- The latest locomotion state is applied from the same sample as the numeric
  transform and speed. Compound changes are staged across
  separate graph updates (`idle -> running -> sprinting`, and the reverse when
  stopping). Transform movement waits for the matching graph revision, and an
  idle checkpoint snaps any remaining positional backlog rather than moving while
  visibly idle. Locomotion variables are refreshed at the local game-frame rate
  with the replicated speed in the same game-thread pass as translation. Stop
  events are sent before clearing their old variables; start events are sent
  after establishing the new variables.
  Graph-variable failures are retried. Rejected events receive four bounded
  attempts before state convergence continues. A newly created or reloaded
  graph receives the current assertion.
- Takeoff first restores `bInJumpState` and the jump sample's graph variables,
  then dispatches `JumpStandingStart` or `JumpDirectionalStart`. Landing aligns
  the actor to the grounded transform checkpoint, restores
  `bInJumpState=false` and the current locomotion variables, and only then
  dispatches `JumpLand`. The applied jump value is also reasserted around each
  normal actor update so the locally grounded controller cannot overwrite it.
  Post-jump locomotion is reasserted after the graph output sink observes
  `JumpLandEnd`.

## POC limitations

1. Appearance (spec 006) and equipped items (spec 007) are synchronized
   separately. Only vanilla items are shown on remote puppets.
2. Cell and worldspace form IDs only work reliably when peers have compatible
   load orders.
3. Interior/worldspace changes use an intentional despawn/respawn boundary;
   continuous presentation through loading screens would need a richer
   transition protocol.
4. There is no authentication, lobby, character identity, persistence, or
   server-authoritative movement.
5. There is no timestamped interpolation buffer, extrapolation, latency
   compensation, or teleport detection. Only the latest snapshot is retained.
6. Basic locomotion, sneak/sprint, and jump transitions are replicated.
   Swimming, combat, directional strafing, and actor values are not;
   unsynchronized local combat and hit reactions are suppressed on puppets.
7. The endpoint defaults to `http://127.0.0.1:3000`. Remote clients configure
   it through `Data/SKSE/Plugins/skyrim-mp-server.txt`; there is no in-game UI.
