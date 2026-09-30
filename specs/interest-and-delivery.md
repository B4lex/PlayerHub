# Interest and Delivery

## Status

Implemented in `server/internal/interest`, `server/internal/hub`, and
`server/internal/delivery`. Covered by rule and grid tests
(`interest_test.go`), hub tests with a fake clock (`hub_test.go`: welcome
first, enter order, an unplaced viewer, a paused sender seen by a newcomer,
batching, the action anchor, context order, range hysteresis and time in view,
exclusions taking effect at once, a space change, presentation kept across exit
and re-enter, disconnect reaching presentation holders, virtual players, chat),
outbox tests, and black-box tests over a real WebSocket, including the admin
hide and exclusion endpoints. Not checked in game.

`cmd/loadgen` with 250 walking bots at 20 Hz on a 4-core host shared with the
load generator:

| Scenario | Spread | Server CPU | Egress | p50 / p99 |
| --- | --- | --- | --- | --- |
| Sparse | ±100000 | 27% | 0.22 MB/s | 14 / 26 ms |
| Dense, all within one radius | ±3000 | 63% | 34 MB/s | 22 / 42 ms |
| Churn, 2000 units/s | ±20000 | 40% | 5 MB/s | 18 / 31 ms |

## Scope

This system decides which peers each client receives and delivers them in an
order the client can rely on. It owns the hub goroutine that applies every
accepted message, subscriptions between recipients and peers, the per-pair
record of presentation revisions, and the per-recipient outbox that batches
checkpoints. Wire messages and validation are in [Network](network.md); how a
client applies them is in [Sync pipeline](sync-pipeline.md#receive-state).

## Components

| Module | Responsibility |
| --- | --- |
| `interest.Rules` | Decides one (viewer, target) pair: space, exclusions, enter and leave radius, time in view |
| `interest.Grid` | Placed players by space and square cell of the leave radius |
| `interest.Hidden`, `interest.Pairs` | The admin exclusion rules |
| `hub.Hub` | All world state; subscriptions; turns accepted messages into outbox traffic. Methods run on one goroutine and take an injectable clock |
| `hub.Loop` | Runs a hub: submitted work in order, a tick at `send_rate_hz`, and the interest pass at `interest.update_hz` |
| `delivery.Outbox` | One recipient's pending traffic: latest-wins checkpoints, ordered messages, latest-wins presentation |
| `delivery.Batch` | One drain of an outbox, written as WebSocket messages |
| `session` writer | Waits for its outbox, drains it, and writes |

Connections, replays, and in-server bots all enter through the same hub
methods (`Connect`, `Handle`, `Disconnect`), so replayed players follow every
rule here.

## Subscriptions

- A player is placed once it is in the world and has a context and a
  checkpoint ([Network](network.md#checkpoint-and-context)). Only placed
  players are compared: a placed recipient (the viewer) against placed peers
  (targets). A viewer that is not placed receives nobody.
- A recipient is a connection with an outbox; replays and bots receive
  nothing but are seen like any peer.
- The rules for a pair, in order:
  1. Different coordinate spaces never see each other. A worldspace compares
     by worldspace ID; an interior (worldspace 0) by exact cell ID.
  2. Exclusions: a player hidden by an admin is seen by nobody (it still sees
     others), and an excluded directed pair never sees.
  3. Distance is horizontal (x, y), since Skyrim loads cells as a horizontal
     grid. A target enters within `interest.enter_radius` (8192) and leaves
     beyond `interest.leave_radius` (9830), but not before
     `interest.min_dwell_seconds` (1) in view, so it does not flap at the edge.
- Rules 1 and 2 are hard: they take effect at once, whatever the time in view.
  Distance is applied by the periodic pass, at `interest.update_hz` (5), which
  reevaluates each placed viewer's subscriptions and the players in the 3×3
  grid cells around it. The grid's cell is the leave radius, so every pair
  within it is found.
- Events evaluate the affected pairs at once instead of waiting for the pass:
  a player becoming placed (as viewer and as target), hiding or showing a
  player, and adding or lifting an exclusion.
- A context that changes the space unplaces the player until its next
  checkpoint, since the stored position belongs to the old space: its
  subscribers get `PeerLeave(OUT_OF_RANGE)`, its own subscriptions end with
  `VIEWER_UNPLACED`, and the next checkpoint places it in the new space.
  Sequences still have to rise across the gap.
- A player's `Exit` ends every subscription to it with `EXITED` and every
  subscription it has with `VIEWER_UNPLACED`.
- A peer's disconnect sends `PeerLeave(DISCONNECTED)` to every recipient that
  receives it or holds its appearance or equipment, and the recipients forget
  their record of it. Its exclusions are dropped too.

## Ordering

1. `Welcome` is the first message a client receives.
2. `PeerEnter` precedes everything else about a peer and carries, in order,
   the peer's appearance and equipment (only when newer than the recipient's
   record), context, and stored checkpoint. A paused game publishes nothing,
   and the stored checkpoint still places the peer at once.
3. An ordered message about a peer (`PeerContext`, `PeerAction`, `PeerLeave`)
   is never overtaken by, and never overtakes, that peer's checkpoints. Before
   one is queued, the hub hands the peer's undistributed checkpoint to its
   subscribers' outboxes, and the outbox moves the peer's pending checkpoint
   into the ordered stream in front of it; `PeerLeave` discards it instead.
4. An action is preceded by the checkpoint it is anchored to: when the
   recipient's newest queued sequence for the peer is below the action's
   `checkpoint_sequence`, the peer's current checkpoint is queued in front of
   it. The client holds an attack until `lastSequence >= checkpointSequence`
   ([Combat presentation](combat-presentation.md#action-stream)).
5. `PeerLeave` resets the peer's action stream on the client, so the next
   action after a new `PeerEnter` starts a new boundary.
6. Appearance and equipment are latest-wins per (recipient, peer): a newer one
   drops the unsent one and is queued at the end, keeping the true order.
7. Chat and results are ordered with everything else in the outbox.

## Records per pair

For each recipient the hub records the appearance and equipment revision of
each peer it has sent, whether or not the peer is subscribed now. `PeerEnter`
includes them only when newer, which matches the client, which keeps
presentation until `PeerLeave(DISCONNECTED)`. The record is dropped on that
leave and when the recipient disconnects.

## Batching

- A checkpoint accepted by the hub is encoded once as a `PeerStates` entry
  (`wire.AppendEntry`) and marked changed. Every tick (`send_rate_hz`, default
  40), each changed sender's entry is set in its subscribers' outboxes,
  replacing an unsent older one, and each touched outbox is signalled once.
- The writer drains the outbox and writes: consecutive entries in the ordered
  stream as one `PeerStates`, every ordered message as its own WebSocket
  message, and the remaining latest-wins entries as one final `PeerStates`. A
  batch is the entries' bytes placed one after another behind one header, so
  each entry is encoded once however many recipients receive it.
- Ordered messages wake the writer at once; checkpoints wait for the tick, so
  batching adds at most one tick of delay. The ticker drops missed ticks
  instead of bursting after a stall.
- A write that takes longer than `liveness.timeout_seconds` closes the
  connection. While a slow client writes, its outbox keeps only the newest
  checkpoint per peer; ordered messages accumulate until
  `limits.ordered_backlog`, which closes the connection with status 4002.

## Admin controls

`PUT /api/players/{id}/hidden {hidden}` hides or shows a player;
`POST /api/exclusions` and `DELETE /api/exclusions` with `{viewerId, targetId}`
add or lift a directed exclusion. Both apply at once and last until the
player disconnects; there is no persistent identity to keep them longer. The
admin state lists each player's `visible` peers, `hidden`, `bytesPerSecond`
written to it, and `replacedPerSecond` (checkpoints replaced before they were
written, a sign of a slow link), and every exclusion.

## Engine assumptions

1. A remote actor within 9830 units of the local player in the same worldspace
   stands in a loaded cell, so it can be spawned (`ActorSpawner::IsSpawnLocationLoaded`).
   A smaller `uGridsToLoad` than the default 5 may leave actors at the edge
   unplaceable until they come closer.

## Acceptance criteria

1. A peer that enters is shown with its look and position before anything else
   about it arrives, including when its game is paused.
2. Attacks play in order and are never held back by a checkpoint that did not
   arrive, including after a peer walks out of range and back.
3. A client that stops reading costs the server bounded memory and is
   disconnected.
4. A peer walking out of range despawns and respawns when it returns, without
   flapping at the edge; an excluded or hidden peer disappears at once.

## Known limitations

- There is no cap on visible peers or rate class yet, so a crowd within one
  radius costs bandwidth with the square of its size.
- Exclusions and hiding last only for a connection; product rules such as
  block lists, parties, or instances need a persistent identity.
- Spaces compare by load-order form IDs, like the client.
