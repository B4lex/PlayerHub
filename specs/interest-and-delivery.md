# Interest and Delivery

## Status

Implemented in `server/internal/interest`, `server/internal/hub`, and
`server/internal/delivery`. Covered by rule and grid tests (`interest_test.go`),
hub tests with a fake clock (`hub_test.go`: welcome first, enter order, an
unplaced viewer, a paused sender seen by a newcomer, batching, the action
anchor, context order, range hysteresis and time in view, exclusions taking
effect at once, a space change, presentation kept across exit and re-enter,
disconnect reaching presentation holders, virtual players, chat on each channel,
a session's display name on the relayed appearance, a session connecting
again, a player's vanish following the session), outbox tests, and black-box tests over a real WebSocket,
including the admin hide and exclusion endpoints. Not checked in game.

`cmd/loadgen` with 250 walking bots at 20 Hz on a 4-core host shared with the
load generator:

| Scenario | Spread | Server CPU | Egress | p50 / p99 |
| --- | --- | --- | --- | --- |
| Sparse | ±100000 | 27% | 0.22 MB/s | 14 / 26 ms |
| Dense, all within one radius, no cap | ±3000 | 63–71% | 34 MB/s | 22 / 42 ms |
| Churn, 2000 units/s | ±20000 | 40% | 5 MB/s | 18 / 31 ms |

With the defaults (cap 64, far peers at 10 Hz) and idle suppression, "near
p99" counting only peers within 4096 units:

| Scenario | Spread | Server CPU | Egress | p50 / p99, near p99 |
| --- | --- | --- | --- | --- |
| Dense, all moving | ±3000 | 56% | 9.0 MB/s | 16 / 36 ms, 35 ms |
| Dense, 54% of bots idle | ±3000 | 50% | 4.1 MB/s | 18 / 31 ms, 31 ms |
| Wide, 54% idle | ±12000 | 48% | 2.4 MB/s | 21 / 58 ms, 34 ms |

With the cap and far rate off, idle suppression alone took the dense layout
with 54% idle bots from 34 to 15.7 MB/s. A far peer's latency includes its
deliberate hold of up to 100 ms.

With the budget for a 50 Mbit/s uplink (`budget.enabled`, 6250000 bytes/s),
all bots moving, "near" within 1024 units, and "gap" the longest time a
visible peer went without an update:

| Scenario | Cap | Server CPU | Egress | p50 / p99, near p99 | Gap p99 / max |
| --- | --- | --- | --- | --- | --- |
| Dense ±3000 | 64 | 68% | 4.45 MB/s | 27 / 68 ms, 68 ms | 274 / 576 ms |
| Dense ±3000 | 32 | 59% | 4.37 MB/s | 21 / 54 ms, 54 ms | 102 / 178 ms |
| Wide ±12000 | 64 | 64% | 4.35 MB/s | 27 / 57 ms, 52 ms | 223 / 327 ms |
| Dense ±3000, no cap or far rate | none | 104% | 4.89 MB/s | 36 / 80 ms | 602 ms / 1.2 s |

Egress stays under the 5 MB/s the budget allows. With 64 peers in view, the
nearest peers' p99 misses 60 ms by 8 ms because the budget is below demand; a
cap of 32 meets it, so the homelab example uses 32.

## Scope

This system decides which peers each client receives and delivers them in an
order the client can rely on. It owns the hub goroutine that applies every
accepted message, subscriptions between recipients and peers, the per-pair
record of presentation revisions, and the per-recipient outbox that batches
checkpoints. Wire messages and validation are in [Network](network.md); how a
client applies them is in [Sync pipeline](sync-pipeline.md#receive-state).
Local chat compares the same spaces and horizontal distance but is routed
without subscriptions ([Network](network.md#chat)), and the hub tells each
client which players it reaches without them
([Network](network.md#local-players)).

## Components

| Module | Responsibility |
| --- | --- |
| `interest.Rules` | Decides one (viewer, target) pair: space, exclusions, enter and leave radius, time in view |
| `interest.Grid` | Placed players by space and square cell of the leave radius |
| `interest.Hidden`, `interest.Pairs` | The admin exclusion rules |
| `hub.Hub` | All world state; subscriptions; turns accepted messages into outbox traffic. Methods run on one goroutine and take an injectable clock |
| `hub.Loop` | Runs a hub: submitted work in order, a tick at `send_rate_hz`, and the interest pass at `interest.update_hz` |
| `delivery.Outbox` | One recipient's pending traffic: latest-wins checkpoints, ordered messages, latest-wins presentation; rate classes and the budget's choice of checkpoints |
| `delivery.Budget` | The uplink shared by every recipient |
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
     others), an excluded directed pair never sees, and a viewer never sees a
     session it vanished (`interest.Vanished`, from `View.Vanished`, matched
     against the target's `View.Session`;
     [Network](network.md#vanish)). Only the vanish follows a session across
     its connections; the admin rules are keyed by slot.
  3. Distance is horizontal (x, y), since Skyrim loads cells as a horizontal
     grid. A target enters within `interest.enter_radius` (8192) and leaves
     beyond `interest.leave_radius` (9830), but not before
     `interest.min_dwell_seconds` (1) in view, so it does not flap at the edge.
     A client may ask for a smaller radius in `Hello.view_radius`
     (`[Performance] ViewRadius` in [Configuration](configuration.md)): it is
     clamped to `interest.minimum_view_radius` (1024) and the server's enter
     radius, and its leave radius keeps the server's ratio.
  4. The cap: a viewer receives at most `interest.max_visible` (64) peers,
     nearest first. A peer in view keeps its place unless a candidate is
     `interest.cap_hysteresis` (1024) nearer, and one within its minimum time
     in view is never evicted (evictions leave with `OUT_OF_RANGE`). The
     periodic pass ranks; an event that would overfill a view waits for it.
     The default is provisional: it should come from measuring the client's
     frame cost with many remote actors in game.
- Rules 1 and 2 are hard: they take effect at once, whatever the time in view.
  Distance is applied by the periodic pass, at `interest.update_hz` (5), which
  reevaluates each placed viewer's subscriptions and the players in the 3×3
  grid cells around it. The grid's cell is the leave radius, so every pair
  within it is found.
- Events evaluate the affected pairs at once instead of waiting for the pass:
  a player becoming placed (as viewer and as target), hiding or showing a
  player, adding or lifting an exclusion, and a `Vanish`, which evaluates the
  viewer against the session's connected player, if any. A lifted vanish
  that finds the view full waits for the pass, as any newcomer does.
- A checkpoint moves the player's view (`Space`, `X`, `Y`) in place; its slot,
  session, radius, and vanished sessions are set on connect and by the hello.
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
   it. The client holds an attack until its played checkpoint reaches `checkpointSequence`
   ([Combat presentation](combat-presentation.md#action-stream)).
5. `PeerLeave` resets the peer's action stream on the client, so the next
   action after a new `PeerEnter` starts a new boundary.
6. Appearance and equipment are latest-wins per (recipient, peer): a newer one
   drops the unsent one and is queued at the end, keeping the true order.
7. Chat, results, roster and local player changes, inspections, and trade messages are
   ordered with everything else in the outbox, so a player receives the
   other player's report before its acceptance and both before a commit
   ([Trade](trade.md#commit)).

## Records per pair

For each recipient the hub records the appearance and equipment revision of
each peer it has sent, whether or not the peer is subscribed now. `PeerEnter`
includes them only when newer, which matches the client, which keeps
presentation until `PeerLeave(DISCONNECTED)`. The record is dropped on that
leave and when the recipient disconnects.

## Rate classes and idle suppression

- A peer farther than `interest.near_distance` (4096) is written to the viewer
  at most `interest.far_rate_hz` (10) times a second; nearer peers get every
  checkpoint. The periodic pass sets each pair's class
  (`Outbox.SetClass`). A far peer's newer checkpoint waits in the outbox
  until due and is never lost; the writer's timer fires one tick after the due
  time, so a tick's batch usually carries it without a separate write. An
  ordered message about the peer still takes its pending checkpoint along at
  once.
- A checkpoint equal to the last one handed to subscribers, except for its
  sequence and time, is not distributed, so an idle player costs nothing on the wire.
  The stored checkpoint still advances, so `PeerEnter` and action anchors carry
  the newest. The checkpoint after a context change is always distributed,
  since clients apply a context with the next checkpoint they receive.

## Budget

Optional (`budget.enabled`), for an uplink that cannot carry the unbudgeted
traffic, such as a 50 Mbit/s homelab. The router's upload buffer must never be
what limits the rate, or every player's latency rises.

- The server sends at most 80% of `budget.uplink_bytes_per_second` (default
  6250000, 50 Mbit/s): a global token bucket with 0.1 s of burst. The rest is
  left for TCP acknowledgements, retransmits, and pings.
- Each recipient's share is the global rate divided by the connected
  recipients, capped by `budget.per_recipient_bytes_per_second` (0, no cap),
  with 0.2 s of burst. What an idle recipient does not use stays in the global
  bucket for the others.
- Every drain charges its ordered messages, which are always sent, to both,
  and 60 bytes per WebSocket message for IP, TCP, and WebSocket headers. The
  latest-wins checkpoints then go in order of priority until the allowance is
  spent: waiting time, starting at 0.025 s, times the pair's
  weight, which is 1 within 1024 units and falls as 1024 / distance, in
  eighths, to 1/8. A checkpoint that does not fit waits with its waiting time,
  so every peer is eventually sent; the writer retries after 25 ms.
- New subscriptions are spread: a viewer gets at most
  `budget.enters_per_second` (20) new peers per second, nearest first, so
  entering a crowd does not send dozens of appearances in one burst.

## Batching

- A checkpoint accepted by the hub is encoded once as a `PeerStates` entry
  (`wire.AppendEntry`) and marked changed. Every tick (`send_rate_hz`, default
  20), each changed sender's entry is set in its subscribers' outboxes,
  replacing an unsent older one, and each touched outbox is signalled once.
- The writer drains the outbox and writes: consecutive entries in the ordered
  stream as one `PeerStates`, every ordered message as its own WebSocket
  message, and the remaining latest-wins entries as one final `PeerStates`. A
  batch is the entries' bytes placed one after another behind one header, so
  each entry is encoded once however many recipients receive it.
- Ordered messages wake the writer at once; checkpoints wait for the tick, so
  batching adds at most one tick of delay. The ticker drops missed ticks
  instead of bursting after a stall.
- At 20 Hz the tick runs at the source's publishing rate, so with frame
  timing some ticks carry no checkpoint of a sender and the next carries only
  the newer of two. Clients absorb that spacing with the [checkpoint
  playout](sync-pipeline.md#checkpoint-playout); 40 Hz halves the batching
  delay at twice the batch count.
- A write that takes longer than `liveness.timeout_seconds` closes the
  connection. While a slow client writes, its outbox keeps only the newest
  checkpoint per peer; ordered messages accumulate until
  `limits.ordered_backlog`, which closes the connection with status 4002.

## Admin controls

`PUT /api/players/{id}/hidden {hidden}` hides or shows a player;
`POST /api/exclusions` and `DELETE /api/exclusions` with `{viewerId, targetId}`
add or lift a directed exclusion. Both apply at once and last until the
player disconnects: they are keyed by slot, not by the player's session
([Network](network.md#sessions)). The
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

- The budget counts WebSocket payload plus an estimated 60 bytes of headers
  per message; TLS adds about 22 bytes more per record.
- The cap's default is provisional until the client's frame cost with many
  remote actors is measured in game.
- Exclusions and hiding are keyed by slot and last only for a connection;
  product rules such as block lists, parties, or instances would key them by
  session.
- Spaces compare by load-order form IDs, like the client.
