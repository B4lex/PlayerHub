# Interest and Delivery

## Status

Implemented in `server/internal/hub` and `server/internal/delivery`, with one
rule: every connected client receives every placed peer. Covered by hub tests
with a fake clock (`hub_test.go`: welcome first, enter order, a paused sender
seen by a newcomer, batching, the action anchor, context order, presentation
kept across exit and re-enter, disconnect reaching presentation holders,
virtual players, chat), outbox tests (`outbox_test.go`), and black-box tests
over a real WebSocket. Load measured with `cmd/loadgen`
([Network](network.md#status)).

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
| `hub.Hub` | All world state; subscriptions; turns accepted messages into outbox traffic. Methods run on one goroutine and take an injectable clock |
| `hub.Loop` | Runs a hub: submitted work in order, and a tick at `send_rate_hz` |
| `delivery.Outbox` | One recipient's pending traffic: latest-wins checkpoints, ordered messages, latest-wins presentation |
| `delivery.Batch` | One drain of an outbox, written as WebSocket messages |
| `session` writer | Waits for its outbox, drains it, and writes |

Connections, replays, and in-server bots all enter through the same hub
methods (`Connect`, `Handle`, `Disconnect`), so replayed players follow every
rule here.

## Subscriptions

- A peer is placed once it is in the world and has a context and a checkpoint
  ([Network](network.md#checkpoint-and-context)). Only placed peers are
  subscribed to.
- A recipient is a connection with an outbox; replays and bots receive
  nothing.
- The rule is: a recipient receives every placed peer but itself. A recipient
  is subscribed when it connects to every placed peer, and to a peer when it
  becomes placed.
- A peer's `Exit` ends every subscription to it with `PeerLeave(EXITED)`.
- A peer's disconnect sends `PeerLeave(DISCONNECTED)` to every recipient that
  receives it or holds its appearance or equipment, and the recipients forget
  their record of it.

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

## Engine assumptions

None; this system is server-only.

## Acceptance criteria

1. A peer that enters is shown with its look and position before anything else
   about it arrives, including when its game is paused.
2. Attacks play in order and are never held back by a checkpoint that did not
   arrive.
3. A client that stops reading costs the server bounded memory and is
   disconnected.

## Known limitations

- Every client receives every placed peer; there is no range, cap, or rate
  class yet, so bandwidth grows with the square of the player count.
