# Network

## Status

- Server: implemented in Go. `go test -race ./...` in `server/` covers
  validation (`internal/validate`), hub ordering and relay (`internal/hub`),
  outboxes (`internal/delivery`), and black-box sessions over a real WebSocket
  (`internal/session`, `internal/admin`). `cmd/loadgen` measured 250 walking
  bots, every one receiving every other, on a 4-core Linux host shared with
  the load generator: 1.24M entries/s, server CPU 58–64% of one core, latency
  p50 22 ms and p99 42 ms from sending a checkpoint to a peer receiving it.
- Plugin transport (`network/ServerClient`, `network/WireCodec`,
  `protocol/Quantization.h`): built on Linux against IXWebSocket 12.0.1 and
  nanopb 0.4.9 with stubbed Skyrim headers, and run against the Go server with
  a loadgen bot as the peer: connect, hello and welcome, enter, context,
  checkpoints, an attack, appearance, equipment, chat and its rejection,
  latency probes, traffic counting, and closing were checked. The Windows
  xmake build and a `wss://`
  connection through a Cloudflare tunnel to the Go server were verified in
  game under wine; other in-game behavior is not verified.
- Not verified in game: the connection policy and recovery
  ([Connection](#connection)), and whether quantization shows as jitter. The
  nickname requirement is not built on Windows either; `ConnectionMonitor.cpp`
  only passed a Linux syntax check with stubbed headers.

## Scope

This system owns the wire protocol, the client transport and connection
management, and the server's sessions, validation, and per-player state. It
validates untrusted fields at the boundary and queues plain values for the
[sync pipeline](sync-pipeline.md#receive-state), which applies them on the game
thread. Which peers a client receives and in what order is
[Interest and delivery](interest-and-delivery.md). Recording and replay are in
[Recording and replay](recording-and-replay.md).

Protocol and server versions ship together with each client release, so the
protocol evolves directly, without compatibility shims. A version mismatch is
refused at the handshake ([Transport](#transport)).

## Components

| Module | Responsibility |
| --- | --- |
| `proto/skyrimmp/v1/` | The schema (`state.proto`, `messages.proto`) and nanopb size options. `proto/generate.sh` regenerates `server/internal/wire/pb` and `src/network/proto` |
| `src/protocol/` | Plain wire values and shared validation: `PlayerState.h`, `MovementState.h`, `AppearanceState.h`, `EquipmentState.h`, `FormReference.h`, `ChatMessage.h`, `PeerSlot.h`, and the quantization rules in `Quantization.h`. No Skyrim headers |
| `network/WireCodec` | nanopb encoding of client messages and validated decoding of server messages |
| `network/ServerClient` | IXWebSocket transport: sends, dispatches received messages, merges each peer's context into its checkpoints, and queues per-player changes for `DrainRemotePlayerChanges` |
| `network/ConnectionMonitor` | When to connect, reconnect schedule, latency probes, refusal, and the connection state |
| `network/TrafficMeter` | Bytes per second in each direction |
| `server/internal/session` | WebSocket accept, hello, reader and writer goroutines, liveness, per-connection limits |
| `server/internal/validate` | Field validation of client messages |
| `server/internal/world` | A player's accepted state and the ordering rules; name indexes |
| `server/internal/wire` | Hand-written encoding of checkpoint entries and batches, quantization for tools |
| `server/internal/hub` | Applies accepted messages on one goroutine ([Interest and delivery](interest-and-delivery.md)) |

## Transport

- One binary WebSocket message carries one `ClientMessage` or `ServerMessage`,
  each a protobuf `oneof`. The server listens at `/ws`.
- `[Network] ServerEndpoint` keeps its `http://` or `https://` form; the client
  maps it to `ws://` or `wss://` and appends `/ws` when the endpoint has no
  path. TLS is terminated by a reverse proxy in front of the server.
- The client's TLS is IXWebSocket 12 on mbedtls, pinned to 3.6 in `xmake.lua`,
  and verifies the server against the Windows certificate store. mbedtls 4
  returns `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` from `mbedtls_ssl_read`
  for every TLS 1.3 session ticket (Cloudflare sends two), which IXWebSocket's
  `SocketMbedTLS::recv` treats as a failed read, so the handshake failed with
  "Failed reading HTTP status line". mbedtls 3.6 ignores tickets unless the
  client opts in. The `package.librarydeps.strict_compatibility` policy
  rebuilds IXWebSocket when mbedtls changes, because IXWebSocket selects the
  mbedtls API at compile time; a stale build linked against 3.6 failed with
  "SSL setup failed".
- The nanopb runtime is built from the `lib/nanopb` submodule at tag `0.4.9`,
  the generator version `proto/generate.sh` pins: the xmake `nanopb` package
  depends on `protoc`, whose install test fails under wine
  (`STATUS_DLL_NOT_FOUND`).
- The first client message must be `Hello{protocol_version, view_radius}` within
  `limits.hello_timeout_seconds` (5). A version other than
  `Protocol.PROTOCOL_VERSION` (2) is closed with status 4001 and reason
  `protocol <server version>`; anything else first is closed with 1008.
- The server answers a hello with `Welcome{slot}`. Slots are `uint32` from a
  counter that only rises and are never reused within a server run, so a slot
  names one connection. Replays and in-server bots get slots too.
- Liveness runs on the transport, not the game: the server pings every
  `liveness.ping_interval_seconds` (5) and closes a connection whose pong does
  not arrive within `liveness.timeout_seconds` (10). IXWebSocket answers pings
  on its own thread, so a paused game stays connected.
- Boundary limits: 16 KiB per message (the largest valid `ClientMessage` is
  about 12.9 KB); `limits.messages_per_second` (120, burst 240) per connection,
  over which the connection is closed with 4003; a recipient whose unsent
  ordered messages exceed `limits.ordered_backlog` (4096) is closed with 4002.
  Compression is off on both ends.

## Client messages

| Message | Handling |
| --- | --- |
| `Hello` | Handshake, above. `view_radius` is the client's `[Network] ViewRadius` in game units, 0 for the server's ([Interest and delivery](interest-and-delivery.md#subscriptions)) |
| `Enter`, `Exit` | World presence. `Exit` when not in the world does nothing |
| `Context` | A checkpoint's space and graph epoch ([below](#checkpoint-and-context)) |
| `Checkpoint` | One quantized sample |
| `Action` | One attack ([Actions](#actions)) |
| `Appearance`, `Equipment` | Latest-wins presentation ([below](#appearance-and-equipment)) |
| `ChatSend` | `{request_id, text, channel}`, answered with a `Result` ([Chat](#chat)) |
| `Ping` | `{request_id}`, answered with `Result{ok}` by the session at once; the latency probe |

A rejected message is dropped and logged on the server (at most one summary
per connection every 5 s); only `ChatSend` and `Ping` get a `Result`. Graph
event names never cross the network.

## Checkpoint and context

A checkpoint is the latest-wins player state, sampled once per 0.05 s
([Sync pipeline](sync-pipeline.md#local-publishing)). Its space and graph
epoch rarely change, so they travel as a separate `Context`, sent when a field
changes and after every `Enter`. A checkpoint belongs to the sender's latest
context.

| `Checkpoint` field | Encoding | Validation |
| --- | --- | --- |
| `player` | sender slot | set by the server on relayed entries; ignored from clients |
| `sequence` | `uint64` | strictly rising per player; `Exit` resets it |
| `x`, `y`, `z` | `sint32`, whole game units | any |
| `heading` | `uint32`, 1/4096 turn | `0..4095` |
| `pitch` | `sint32`, radians × 1024 | `|pitch| <= 1609` (π/2) |
| `speed` | `uint32`, units per second | `<= 100000` |
| `direction` | `uint32`, graph `Direction` × 256, wrapping | `0..255` |
| `flags` | bits 0–2 movement state `0..6` (`idle`, `walking`, `running`, `sprinting`, `sneakingIdle`, `sneakingWalking`, `sneakingRunning`); bits 3–11 `jumping`, `weaponDrawn`, `bowDrawn`, `castingLeft`, `castingRight`, `blocking`, `dead`, `mounted`, `swimming` | unknown bits rejected |
| `time` | `uint32`, the sender's `steady_clock` in milliseconds when sampled, modulo 16384 | `0..16383` |

| `Context` field | Validation |
| --- | --- |
| `cell_id`, `worldspace_id` | any `uint32`; worldspace `0` is an interior |
| `graph_epoch` | `>= 1`, never decreasing while in the world |
| `furniture` | occupied placed reference as a [form reference](#form-references), unset for none |

`protocol/Quantization.h` and `server/internal/wire/quantize.go` hold the same
rules: positions and speed round to whole units, heading is normalized to
[0, 2π) first, pitch is clamped, and direction wraps (1.0 is 0). `time` is wall
time, not accumulated frame time, so a paused or hitching source does not
stretch it; receivers play checkpoints back at its spacing
([Sync pipeline](sync-pipeline.md#checkpoint-playout)). Sixteen seconds of range
keep it within two bytes; receivers recover the wraps from arrival times. The
server does not interpret it, and idle suppression ignores it with the
sequence ([Interest and delivery](interest-and-delivery.md#rate-classes-and-idle-suppression)).
Virtual players and `cmd/loadgen` bots stamp their own
([Recording and replay](recording-and-replay.md#virtual-player)). The server
accepts a `Context` or `Checkpoint` only in the world, and a checkpoint only
after a context since the last `Enter` (`no context`). A context identical to
the stored one is dropped. The meaning of each field is in its owning system:
movement in [Locomotion](locomotion.md), stance and hands in
[Combat presentation](combat-presentation.md), furniture, mounts, and death in
[Native interactions](native-interactions.md), swimming in
[Remote actor](remote-actor.md#swimming).

Cell and worldspace IDs are load-order FormIDs and need the same plugins in
the same order on every peer. Furniture is plugin-qualified.

## Actions

`Action` carries one attack as a separate ordered stream, since a checkpoint
field would collapse repeated attacks.

| Field | Validation |
| --- | --- |
| `action_sequence` | `>= 1`, exactly the previous plus one per player; the first one a server sees sets the boundary |
| `graph_epoch` | equal to the player's context epoch |
| `checkpoint_sequence` | `>= 1`, a published checkpoint no newer than the latest |
| `action` | `NORMAL`, `LEFT`, `DUAL`, or `BASH` |
| `power` | boolean |
| `direction` | `STANDING`, `FORWARD`, `BACKWARD`, `LEFT`, `RIGHT` when `power`; `NONE` otherwise |

The server also requires the latest checkpoint to have the weapon drawn and the
context to have no furniture, and allows `limits.actions_per_second` (10, burst
10) per player. A duplicate, stale, or gapped sequence is rejected rather than
played out of order. The vocabulary's meaning is in
[Combat presentation](combat-presentation.md#action-stream).

## Appearance and equipment

Both are reconstructable state that changes rarely, so each has its own
latest-wins message with a source-local `revision` (`>= 1`, strictly rising
per connection). Capture, resolution, and fallbacks are in
[Appearance and equipment](appearance-and-equipment.md).

### Form references

Items, races, head parts, and furniture are `FormRef{plugin, form_id}`: the
file that defines the form and its file-local ID (the low 24 bits, or 12 for a
light plugin). A receiver resolves by plugin name, so load orders need not
match. Plugin names are 1..260 bytes and `form_id` is `0..0xFFFFFF`. Dynamic
(`FF`) forms cannot be referenced and are omitted.

### `Appearance`

| Field | Validation |
| --- | --- |
| `name` | the player's [nickname](appearance-and-equipment.md#nickname): at most 128 UTF-8 bytes, no control characters (below U+0020, U+007F); empty allowed, though the client never publishes one |
| `level` | `0..65535`, `0` unknown |
| `race` | required form reference |
| `female` | boolean |
| `weight` | finite, `0..100` |
| `height` | finite, `0.25..4` |
| `head_parts` | at most 16, each with a form and `type` `0..6` |
| `hair_color` | RGB `0..255`, unset for none |
| `face_details` | form reference, unset for none |
| `body_tint` | required, RGB `0..255` |
| `face_morphs` | exactly 19 finite floats; unset morphs hold `FLT_MAX` |
| `face_parts` | exactly 4 `sint32` |
| `tints` | at most 64; `index` `0..65535`, `type` `0..14`, a color, `alpha` `0..1` |
| `default_race` | `{race, female}` with a base-game race, unset for none |

Hair color is RGB rather than a form, because character editors commonly
create dynamic color forms. The relayed appearance adds `name_index`
([Name index](#name-index)).

### `Equipment`

| Field | Validation |
| --- | --- |
| `armor` | at most 32, unique references; `slot_mask` non-zero (bit 0 is slot 30) |
| `right_hand`, `left_hand` | a hand, unset for empty |
| hand `kind` | `WEAPON`, `TORCH` (left hand only), or `SPELL` (no form, no weapon type) |
| hand `weapon_type` | `WEAPON_TYPE` `1..9`, only for a weapon; two-handed types (5, 6, 7, 9) only in the right hand, which then requires an empty left hand |
| `ammo` | form reference, unset for none |
| `default_outfit` | unset is empty |
| outfit `armor` | at most 8, non-zero slot masks |
| outfit `hands` | at most 2, a weapon with a weapon type or a torch, no two with the same kind and weapon type |
| outfit references and `default_race.race` | from a base-game plugin, compared without case |

`slot_mask` and `weapon_type` are hints that select the default outfit piece;
the receiver's locally resolved form decides how an item is equipped. The
base-game plugins are the Special Edition masters and the 74 Creation Club
plugins the Anniversary Edition `Skyrim.ccc` lists;
`Multiplayer::IsBaseGamePlugin` and the server's `baseGamePlugins` hold the
same list.

## Server messages

| Message | Fields |
| --- | --- |
| `Welcome` | `slot` |
| `Result` | `request_id`, `ok`, `error` |
| `PeerStates` | `repeated Checkpoint` with `player` set; the concatenation of entries encoded once per sender |
| `PeerEnter` | `slot`, `appearance` and `name_index` and `equipment` when newer than what the recipient holds, `context`, `state` (the stored checkpoint) |
| `PeerContext` | `slot`, `context` |
| `PeerLeave` | `slot`, `reason`: `OUT_OF_RANGE`, `EXCLUDED`, `EXITED`, `DISCONNECTED`, `VIEWER_UNPLACED` |
| `PeerAction`, `PeerAppearance` (with `name_index`), `PeerEquipment` | `slot` and the message |
| `ChatMessage` | `channel`, `sender`, `recipient` (whisper only), `text`; a `ChatSpeaker` is `slot`, `name`, `name_index`, `level` |

When each is sent and in what order is
[Interest and delivery](interest-and-delivery.md#ordering).

## Server

- A player is in the world between `Enter` and `Exit`, and placed once it also
  has a context and a checkpoint. `Exit` forgets the context, checkpoint,
  sequences, and action stream; appearance and equipment are kept.
- A connection that closes, for any reason, is removed from the hub: its
  capture ends, and every recipient that received it or holds its presentation
  gets `PeerLeave` with `DISCONNECTED`.
- Messages are decoded and validated on the connection's reader goroutine and
  applied in arrival order by the hub. `Ping` is answered by the session itself.

### Name index

A player gets an index when its first appearance arrives or its name changes.
Names compare case-insensitively. The player gets the lowest index from 1 not
held by another connected player with that name, and keeps it while the name
stays the same, so indexes are never renumbered when someone leaves. Replayed
and load-test players are indexed the same way. The index is relayed but not
stored in the appearance, so recordings do not contain it.
[Appearance and equipment](appearance-and-equipment.md#player-label) shows it
in the label.

### Chat

`ChatChannel` is `GLOBAL` (0), `LOCAL`, or `WHISPER`.

- The server trims surrounding spaces and rejects an unknown channel, empty
  text, control characters, and more than 512 UTF-8 bytes
  (`invalid chat message`); protobuf already guarantees valid UTF-8.
- `GLOBAL` goes to every connected client.
- `LOCAL` needs a placed sender (`not in world` otherwise) and goes to every
  placed client in the same space: in a worldspace within `chat.local_radius`
  (8192, horizontal like interest), and in an interior to the whole cell. It
  ignores exclusions, hiding, and the visible cap, which govern presentation
  only. Recipients are found by a scan of connected clients, which the rate
  limit keeps cheap; the interest grid does not bound an interior.
- `WHISPER` text starts with the recipient: `world.Names.Find` takes the
  longest name of a connected player, compared case-insensitively and followed
  by a space or the end, then an optional ` #N` for its
  [name index](#name-index) (1 without one). Longest-first lets names contain
  spaces. No match, a missing index, or a replay or bot is
  `unknown recipient`; nothing after the name is `empty whisper`. The relayed
  `text` is the message after the name, and the whisper goes to the sender and
  the recipient (once when they are the same).
- 5 accepted messages per player in any 5 s over all channels
  (`rate limited`); rejected messages do not count, and a disconnect forgets
  the history.
- Every channel includes the sender, so everyone sees one order and the
  sender's own line confirms delivery. A `ChatSpeaker` comes from the player's
  latest appearance (empty name, index `1`, and level `0` before one).
- The client rejects a `ChatMessage` without a sender, with an unknown channel,
  or with a recipient present exactly when the channel is not `WHISPER`, and
  validates speakers like appearance names and text like `ChatSend`.
- Chat does not require being in the world except for `LOCAL`, keeps no
  history, and is not recorded. The client side is in
  [UI overlay](ui-overlay.md#chat).

## Client

- `ServerClient` runs its callbacks on the IXWebSocket thread. `WireCodec`
  validates each message as the server does; a rejected one is logged with the
  offending field, and plain values are queued; nothing touches game objects.
  The nanopb `ServerMessage` buffer is allocated once per thread, and
  `PeerStates` is decoded entry by entry (at most 4096).
- Nothing before `Welcome` is accepted. `Welcome` advances the connection
  generation and clears all receive history.
- Per peer, the transport keeps the latest context from `PeerEnter` and
  `PeerContext` and merges it into each checkpoint, so the sync pipeline still
  receives complete `PlayerTransform` values. A checkpoint of a peer without a
  context (no `PeerEnter` yet) is dropped, and so is a context whose epoch
  falls.
- Only the newest pending checkpoint (by sequence), appearance, and equipment
  (by revision) per peer are kept until the pass drains them. A checkpoint is
  stamped with its `steady_clock` arrival, once per `PeerStates` or
  `PeerEnter`, for the playout clock. `PeerEnter`
  starts a fresh checkpoint and action stream. `PeerLeave` forgets the peer's
  context, checkpoint, and action stream; with `DISCONNECTED` it also forgets
  its presentation. Actions are queued in order and a gap is rejected.
- `ChatMessage` payloads are queued, at most 128, dropping the oldest, and
  kept across a lost connection. The transport marks a message `outgoing` when
  its sender is the connection's own slot, so a whisper can show its
  direction.

### Latest-wins publishers

Every welcome advances the connection generation. Appearance and equipment
republish their last state on each new generation, because the server forgets
a connection's state when it drops; `PresenceSync` sends `Enter` again, and
the next checkpoint sends its context. Remote players are then rebuilt from
the server's `PeerEnter` messages.

### Connection

A connection is wanted only in a game, while the player's choice enables it,
and once `[General] Nickname` is set
([Appearance and equipment](appearance-and-equipment.md#nickname)).
`ConnectionMonitor` owns that decision; nothing else opens or closes the
client.

- At `kDataLoaded`, `Network::Initialize` only initializes the socket library;
  the main menu never connects.
- A game starts when `PresenceSync` first finds the player in the world, and
  ends when the main menu opens (`GameSession`). Loading screens keep the game
  and its connection.
- At a game start the choice becomes `[Network] AutoConnect` (default `true`).
  The settings window's Connect and Disconnect buttons change it with
  `Network::SetConnectionEnabled` until the game ends.
- The monitor reads the nickname when the configuration revision changes and
  publishes `Network::HasNickname` for the UI. With the choice enabled, applying
  a first nickname opens the connection at once, and applying an empty one
  closes it like Disconnect. The requirement is the client's: the server still
  accepts an empty `Appearance.name`.
- Opening skips the reconnect delay and resets the attempt count. Closing
  (`BeginClose`) marks the client disconnected at once, forgetting every remote
  player, and stops the socket on a worker, which joins the socket's thread. It
  shares the worker slot with `BeginReconnect`, so a busy attempt keeps the
  monitor in its closing phase, retried each pass. A welcome that still
  arrives after a close was requested is ignored.
- `Network::GetConnectionState` never runs ahead of the transport: `kConnected`
  once welcomed, `kConnecting` while one is wanted but not welcomed,
  `kDisconnecting` while none is wanted but the socket is open, and
  `kDisconnected` otherwise.
- An applied endpoint that differs from the latest attempt's abandons a live
  connection and reconnects at once, with the attempt count reset.
- A close with status 4001 is a refusal: the monitor stops reconnecting,
  `Network::GetConnectionRefusal` returns the reason for the status panel and
  the settings tooltip, and only an endpoint change or pressing Connect again
  retries.

The monitor's timers accumulate the clamped frame delta, which is zero in the
main menu and while paused, so they hold while opening and closing still follow
the game.

### Loss and recovery

- A server restart, a missed server ping, or a network loss reaches the client
  as a WebSocket close or error. Each marks the client disconnected and forgets
  every remote player.
- A server that stops answering while the socket stays open is found by the
  latency probe: no `Result` for 5 s of accumulated frame time makes the
  monitor call `AbandonConnection`, which acts like a reported close.
- IXWebSocket's own reconnection is disabled; each loss is reported once, and
  the monitor reopens.
- After a loss the monitor waits 1 s, then calls `BeginReconnect`, doubling the
  wait after each failed attempt up to 15 s, each wait scaled by a random factor
  in 0.5–1.5 so clients that lost one server do not retry together. Attempts are
  unlimited. An attempt reported failed, or not welcomed after 20 s, is followed
  by the next. The WebSocket handshake times out after 10 s.
- `BeginReconnect` runs on a worker: stopping the old socket joins its thread,
  so nothing from the dead connection is reported afterwards, then a new socket
  starts.

### Latency

The client sends `Ping` 1 s after the previous one was sent, with at most one
outstanding. The round trip is measured with `std::chrono::steady_clock` from
send to `Result`, so frame rate does not affect it. Each ping has a request ID;
a compare-exchange on the outstanding probe accepts only the current probe's
answer, and requests are forgotten with their connection. A disconnect or new
generation clears the sample.

### Traffic

`TrafficMeter` counts each WebSocket message at the size IXWebSocket reports on
the wire (`wireSize`), in each direction. TLS, TCP, and ping frames are not
counted. Rates are averaged over the latest complete one-second wall-clock
window, and are empty after a long gap. The status panel shows them.

## Engine assumptions

- IXWebSocket's thread stops cleanly inside the game process at exit; only
  starting it and closing a connection on request were observed.

## Acceptance criteria

1. Two clients see each other move, attack, sit, ride, die, and change
   appearance and equipment as before the protobuf transport.
2. A client that joins while a peer's game is paused sees that peer at once.
3. A client with another protocol version shows `Server version differs` and
   does not retry until Connect or an endpoint change.
4. A paused game (menu open) stays connected for minutes.
5. Quantization shows no visible jitter in position or heading.
6. A client without a nickname opens no connection, whatever `AutoConnect` and
   Connect say; applying one connects, and applying an empty one disconnects.

## Known limitations

- There is no authentication. The admin API can delete recordings and spawn
  virtual players, so the server port must not face the Internet.
- Chat has no history for late joiners.
- Cell and worldspace IDs need matching load orders.
- Non-goals: server authority over movement, anti-cheat, persistent identity,
  and UDP.
