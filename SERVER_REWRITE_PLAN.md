# Server rewrite plan: interest-filtered delivery

Status: phase 0 done 2026-09-30; phase 1 in progress. Written 2026-09-29 from
code review and benchmarks run that day; [gaps](#gaps-resolved-before-execution)
found in a second review on 2026-09-30 are resolved below and folded into the
phases. This file is a work plan, not a spec: when a phase
lands, describe the result in `specs/` (see [Specs to update](#specs-to-update))
and trim this plan.

## Goal

Replace the relay that sends every player to everyone with **per-recipient
delivery**. Each client receives only the players it may see, at a rate and
level of detail chosen per recipient. Proximity is the first rule. The pipeline
is groundwork for later rules, and **exclusion rules are the priority**.
Appearance and equipment become range-based too.

Targets:

- 250 players per server.
- Hosting on a homelab with a 50 Mbit/s upload for now, later a remote server
  with a 1 Gbit/s link. The difference between the two is configuration only.
- Latency no worse than today. p99 from a sender's `Checkpoint` to a
  recipient's receipt stays under 60 ms on the server's local network at
  250 players.

Non-goals: server authority over movement, authentication and persistent
identity, anti-cheat, and UDP. UDP is considered in a
[later phase](#phase-5-later-and-optional) only.

## Decisions

| Decision | Choice | Main evidence |
| --- | --- | --- |
| Server language | **Go** | Go used 1.2–1.45× Rust's CPU at the same latency. Both fit 250 players within one core. [Go vs Rust](#go-vs-rust) |
| Wire format | **protobuf** with quantized fields | 21.6 B per entry vs 452 B of JSON. [Entry sizes](#entry-sizes) |
| Plugin protobuf library | **nanopb**, generated code checked in | Same wire format as protobuf, no runtime library, available in xmake-repo |
| Transport | **Binary WebSocket frames**, socket.io removed | socket.io only carries JSON text and adds a layer the new server does not need. UDP deferred |
| Fan-out | **Encode each entry once, then concatenate per recipient** | Re-encoding for each recipient is about 300× more expensive. [Microbenchmarks](#microbenchmarks) |
| State ownership | **One hub goroutine** owns world state. Connections decode and validate in parallel | Deterministic and easy to test. The hub spent 2.75 ms of a 25 ms tick at 512 dense players |
| Link-dependent limits | **Configuration**: uplink budget, send rate, visible cap, radii | Homelab and remote server differ only in these values |
| Server location | **`server/`**, beside the Python code until phase 1 retires it | No rename at cutover; `admin-ui/` and `recordings/` keep their paths |
| Plugin WebSocket library | **IXWebSocket v12**, TLS through mbedtls | In xmake-repo, maintained, its own I/O thread and ping timer. websocketpp's last release is from 2020 and needs Boost and Asio |
| Go WebSocket library | **`github.com/coder/websocket`**, compression off | Maintained, context-based, concurrent-safe `Ping` |
| Admin feed | **Server-sent events** of JSON snapshots | One-way push; `EventSource` reconnects by itself and passes the Vite proxy |
| Server configuration | **TOML file** plus `--config` and `--listen` flags | Commented, human-edited; one small dependency |

### Premises corrected by the code review

- The current server is not a pure relay. It decodes each `player:update`,
  validates every field (`protocol.py:parse_player_update`), rebuilds the dict,
  adds `playerId`, stores it, and encodes it again inside the batch. Its
  efficiency comes from two things:
  - latest-wins coalescing per 25 ms tick
  - python-socketio encoding a broadcast once and sending the same bytes to
    every client (`socketio/async_manager.py`). Every client also receives its
    own entry.
- Engine.IO's send queue per client is an unbounded `asyncio.Queue`, so a slow
  client makes server memory grow. Measured: 639 MB of memory and 1.5 s of
  latency when the server saturated.
- The client requests an acknowledgement only for `chat:send` and
  `latency:probe`. `player:enter`, `player:exit`, `player:update`,
  `player:appearance`, `player:equipment`, and `animation:action` are sent
  without one.
- Doc mismatches, fixed in [phase 0](#phase-0-preparation):
  - `server/README.md` says `sentAtMs` is relayed and the latency ack carries
    `serverTimeMs`. The code drops `sentAtMs` and acknowledges with
    `{"ok": true}`.
  - `specs/network.md` says "every client event is acknowledged". The client
    emits `player:update` and `animation:action` without an ack ID, so no ack
    is sent.

## Evidence

All benchmarks ran on loopback on an AMD Ryzen 9 9900X (24 threads). The load
generator ran on the same machine, in separate processes: 16 processes of raw
Engine.IO v4 clients using the `websockets` library. Each client sent a real
checkpoint of about 456 B at 20 Hz. Latency is the time from sending a
checkpoint until the sender sees its own entry come back, so it includes the
batching wait of up to 25 ms. The prototype servers implemented only
checkpoints, with no actions, appearance, or recording. They exist to measure
fan-out and transport cost, not as a base for the code.

### Current Python server (sends everything to everyone)

| Players | CPU (1 core = 100%) | Egress | p50 / p99 |
| --- | --- | --- | --- |
| 208 | 41% | 386 MB/s | 16 / 32 ms |
| 304 | 57% | 823 MB/s | 19 / 32 ms |
| 400 | 79% | 1.42 GB/s | 19 / 51 ms |
| 512 | 91% (saturated) | 2.13 GB/s | 91 / 138 ms |

A profile shows the time goes mostly to the per-message overhead of uvicorn,
websockets, Engine.IO, and socket.io, not to the application code.

### Filtered prototypes

"Sparse" means each player sees 1–2 others. "Dense" means about 50 visible for
512 players, or about 100 visible for 1008 players.

| Server | Load | CPU | Egress | p50 / p99 |
| --- | --- | --- | --- | --- |
| Python, filtered | 512 sparse | 70% | 16.5 MB/s | 22 / 33 ms |
| Python, filtered | 512 dense | saturated | 201 MB/s | 1552 / 2803 ms |
| Rust | 512 sparse | 30% | 16 MB/s | 12 / 26 ms |
| Rust | 512 dense | 58% | 255 MB/s | 14 / 30 ms |
| Rust | 1008 dense | 189% | 977 MB/s | 24 / 39 ms |
| Go | 512 sparse | 41% | 16.5 MB/s | 17 / 25 ms |
| Go | 512 dense | 83% | 256 MB/s | 14 / 27 ms |
| Go | 1008 dense | 233% | 986 MB/s | 24 / 35 ms |
| Rust, sends everything | 512 | 345% | 1.9 GB/s | 75 / 115 ms |

- The Python prototype saturated in the dense case because it recomputed
  visibility for every recipient on every tick: 43% of CPU was in that query.
  Recomputing visibility at about 5 Hz fixes that, but Python still needs about
  70% of a core at 512 players before doing any filtering at all.
- Sending everything costs N² bandwidth in any language.

### Microbenchmarks

Python 3.14, stdlib `json`, times per operation:

| Operation | Time |
| --- | --- |
| Decode an incoming Socket.IO event | 4.6 µs |
| `parse_player_update` | 1.5 µs |
| JSON-encode one relayed entry | **5.8 µs** |
| Join an already-encoded entry into a batch | **0.017 µs** |

### Go vs Rust

- A 6 s CPU profile of the Go prototype at 512 dense players:

  | Where the time goes | Share of CPU |
  | --- | --- |
  | WebSocket writes (27% is the `writev` system call itself) | 37% |
  | Ingest (read, JSON decode, validate) | 16% |
  | Hub | 16% |
  | Garbage collector | 16% |
  | Goroutine scheduling | about 10% |
  | JSON decoding | a few % |

- The gap to Rust is the garbage collector and runtime, about a quarter of the
  CPU. Most of the collector's work came from the prototype allocating a new
  batch buffer for every recipient on every tick. Pooled buffers and 20×
  smaller protobuf batches should shrink it (not measured).
- 250 filtered players produce about 0.25–0.5M entries/s. Go handled about
  540k entries/s at 0.83 of a core.
- Reconsider Rust only in these cases:
  - several thousand players on one machine, where CPU becomes the limit
  - garbage-collector pauses showing in p99 after the allocation fixes
  - expensive per-pair computation added to the hot path

  The protobuf schema is the contract with the client, so the hub can be
  rewritten later without changing the client.

### Entry sizes

Measured over 6,149 real checkpoints in `server/recordings`, with Python
`protobuf` and a generated schema:

| Encoding of one relayed entry | Mean | Max |
| --- | --- | --- |
| JSON (today, with `playerId`) | 452 B | 484 |
| protobuf, direct field translation | 61 B | 71 |
| **protobuf, quantized** ([schema](#quantized-checkpoint)) | **21.6 B** (23.4 B while moving) | 30 |

- **44.6% of checkpoints are identical to the previous one after
  quantization**, because the player is idle.
- Fields that changed from one checkpoint to the next:

  | Field | Changed in |
  | --- | --- |
  | `speed` | 62% |
  | position | about 50% |
  | `heading` | 29% |
  | `direction` | 20% |
  | movement state | 1.7% |
  | furniture | 0.7% |

- A hand-written bit-packed format would be about 19 B, not worth its
  maintenance cost.

### Bandwidth model

- Fixed overhead is about 57 B per packet: IPv4, TCP with timestamps, the
  WebSocket header, and the envelope. TLS adds about 22 B.
- Keep 20% of the link in reserve for appearance, equipment, actions, pings,
  TCP ACKs, and retransmits.
- "Entries/s" below is how many peer updates one client can receive per second.

| Scenario at 250 players | 50 Mbit/s homelab | 1 Gbit/s remote |
| --- | --- | --- |
| JSON, sent to everyone | caps at about 26 players | caps at about 105 players |
| protobuf, sent to everyone | caps at about 114 players | 118 MB/s = 94% of the link; caps at about 460 |
| protobuf, 50 visible, idle suppressed | needs 7.6 MB/s, more than the link: needs the [budget scheduler](#phase-4-budget-scheduler) | 7.6 MB/s = 6% (about 20 TB/month at constant load) |
| Budget per client | about 10 KB/s ≈ 380 entries/s at a 20 Hz send rate | about 200 KB/s |

Consequences:

- protobuf is required for both links.
- The homelab needs a hard byte budget per recipient.
- On 1 Gbit/s, the limit on visible peers comes from the Skyrim client's
  actor cost, not the network. Spawns are limited by
  `[Performance] SpawnsPerSecond` (default 20), and each remote actor costs a
  per-frame update.
- If the remote server's traffic is billed per GB, filtering reduces the
  monthly volume by about 15×.

## Architecture

```text
per connection (goroutines)
  reader: frame -> protobuf decode -> validate -> quantized entry encoded once -> hub
  writer: bounded latest-wins mailbox -> builds each batch from the newest entries -> socket

hub (one goroutine; owns all state; later split by space if needed)
  world:     latest state and context per player, spatial grid per space,
             appearance and equipment with revisions
  interest:  every ~200 ms, and at once on enter, exit, space change, or rule change:
             candidates from the grid (same space)
             -> exclusion rules (hard, take effect at once)
             -> distance with separate enter and leave radii, minimum time in view
             -> rank and cap (nearest first, later party priority)
             -> per-pair delivery class (rate, later field subset)
             -> changes -> PeerEnter / PeerLeave
  delivery:  every tick: for each changed sender, for each subscriber, apply
             the pair's rate, mark the entry changed in the recipient's
             mailbox; later the budget scheduler picks entries up to the
             recipient's byte budget
  ordered:   actions, appearance, equipment, context go to current subscribers
             in hub order
```

Proposed Go layout (in `server-go/` during the migration, moved to `server/`
when Python is retired):

| Package | Responsibility |
| --- | --- |
| `cmd/server` | Entry point, configuration |
| `cmd/loadgen` | External load generator: bots as real clients, replays recordings or synthetic walks, measures latency between peers |
| `cmd/convert-recordings` | One-shot conversion of v2 JSONL recordings to v3 |
| `internal/wire` | Generated protobuf, plus hand-written `protowire` appenders for hot messages |
| `internal/validate` | Boundary validation. The cases in `server/tests/test_protocol.py` are ported as table tests |
| `internal/session` | WebSocket accept, reader and writer, liveness, mailbox |
| `internal/world` | Player state, spatial grid, revisions |
| `internal/interest` | Rule interface, built-in rules, enter and leave hysteresis |
| `internal/delivery` | Per-pair rate, entry cache, mailbox filling, later the budget scheduler |
| `internal/recording`, `internal/playback` | Capture, virtual players (same input interface as connections) |
| `internal/admin` | REST API as today, plus a JSON WebSocket feed for `server/admin-ui` |

### Rule interface

A rule is either an exclusion or a delivery setting. Rules are evaluated for a
(viewer, target) pair, in the hub, with no I/O:

```go
type Exclusion interface { Excludes(viewer, target PlayerView) bool }            // hard, checked first
type Classifier interface { Classify(viewer, target PlayerView, d float64) Class } // rate, later field subset
```

- An exclusion removes the target from everything the viewer receives:
  checkpoints, context, actions, appearance, equipment.
- Chat stays global unless a rule explicitly covers it.
- When exclusion state changes, only the affected pairs are re-evaluated at
  once, and a `PeerLeave` with reason `EXCLUDED` is sent. The periodic pass is
  not awaited.

First exclusion rules:

1. Different coordinate space. A worldspace compares by worldspace ID; an
   interior compares by exact cell ID.
2. Admin-hidden player (spectator or invisible admin), set through the admin API.
3. Admin exclusion of one pair (A never sees B), set through the admin API.
   It proves the pairwise path without needing persistent identity.

Product rules such as block lists, parties, or instances need a persistent
player identity, which is a separate decision.

## Wire protocol (draft)

- One binary WebSocket message carries one `ClientMessage` or `ServerMessage`
  envelope, each a `oneof`. The schema lives in `proto/skyrimmp/v1/`. Go and
  nanopb code is generated from it and checked in.
- `[Network] ServerEndpoint` keeps the `http://` and `https://` form; the client
  maps it to `ws://` and `wss://`.

Client → server:

| Message | Fields and notes |
| --- | --- |
| `Hello` | Protocol version; later the client's view radius. The server rejects a version mismatch |
| `Enter` / `Exit` | World presence, as today |
| `Checkpoint` | [Quantized checkpoint](#quantized-checkpoint) without `player` |
| `Context` | `cell_id`, `worldspace_id`, `graph_epoch`, `furniture` (FormRef or unset). Sent when a field changes and after every `Enter`. A checkpoint's space is the latest context |
| `Action` | `action_sequence`, `graph_epoch`, `checkpoint_sequence`, `action` enum, `power`, `direction` enum. The constant `prerequisites` object is dropped; the server still checks the stored checkpoint |
| `Appearance`, `Equipment` | Today's fields and validation. `revision` stays |
| `Chat` | `request_id`, `text` |
| `Ping` | `request_id` |

Server → client:

| Message | Fields and notes |
| --- | --- |
| `Welcome` | The client's own `slot` |
| `Result` | `request_id`, `ok`, `error`. Replaces socket.io acks for `Chat` and `Ping` |
| `PeerStates` | `repeated` quantized entries with `player` = slot. A batch is the pre-encoded entries placed one after another |
| `PeerEnter` | `slot`, then optionally `appearance` (with `name_index`) and `equipment` if newer than what this client holds, then `context`, then `state` (the stored latest checkpoint) |
| `PeerContext` | `slot` plus context, when it changes |
| `PeerLeave` | `slot`, reason: `OUT_OF_RANGE`, `EXCLUDED`, `EXITED`, or `DISCONNECTED` |
| `PeerAction`, `PeerAppearance`, `PeerEquipment` | With `slot`; sent to current subscribers only |
| `ChatMessage` | `slot`, `name`, `name_index`, `level`, `text`. Global |

- Slots are `uint32` values from a counter that only goes up and are never
  reused within a server run. The client keys peers by slot instead of the
  socket.io sid string.
- Liveness: the client sends `Ping` every second, as the latency probe does
  today. The server closes a connection with no incoming message for 10 s.

### Quantized checkpoint

| Field | Type | Encoding and validation |
| --- | --- | --- |
| `player` | `uint32` | Slot (server → client only) |
| `sequence` | `uint64` | Strictly rising. A varint's size depends on the value, not the type, so no wrap-around is needed |
| `x`, `y`, `z` | `sint32` | Game units, rounded. Verify visually; if jitter shows, use 1/4 unit (about +1 B per coordinate) |
| `heading` | `uint32` | `0..4095`, one step is 2π/4096. The client normalizes to [0, 2π) |
| `pitch` | `sint32` | Radians × 1024, `|value| <= 1609` (π/2) |
| `speed` | `uint32` | Units per second, with an upper bound (proposed 100000) |
| `direction` | `uint32` | `0..255` |
| `flags` | `uint32` | Bits 0–2 movement state `0..6`. Bits 3–11: `jumping`, `weaponDrawn`, `bowDrawn`, `castingLeft`, `castingRight`, `blocking`, `dead`, `mounted`, `swimming`. Unknown bits are rejected |

`graphEpoch` moves to `Context`. The server checks that the epoch never
decreases, as today.

## Invariants

1. **The server sends `PeerEnter` before anything else about that peer to a
   recipient, and it carries the stored state.** A paused game publishes
   nothing, because `CheckpointPublisher` stops while the frame delta is zero.
   A newcomer must still see a paused player at once.
2. **`PeerEnter` sends appearance, then equipment, then context, then state.**
   Spawning waits for `AppearanceSync::GetSpawnBase`.
3. **The server sends the checkpoint an action is anchored to.** Before relaying
   an action, the hub sends the sender's latest checkpoint to every subscriber
   whose last delivered sequence is below `checkpoint_sequence`. This holds even
   if idle suppression, rate classes, or the budget would have held it back.
   Then the hub sends the action to those subscribers only. The client holds an
   attack until `lastSequence >= checkpointSequence`
   (`sync/combat/CombatSync.cpp`).
4. **`PeerLeave` resets the peer's action stream on the client.** Otherwise one
   action that was filtered out leaves a gap, and every later action from that
   peer is rejected (`network/SocketIOClient.cpp`, action gap check). The
   `player:exit` handling already does this through
   `ForgetRemoteWorldStateLocked`.
5. **The server's per-recipient record of appearance and equipment revisions
   matches what the client holds.** The client keeps them until
   `PeerLeave(DISCONNECTED)`. The server forgets its record on that leave or
   when the recipient disconnects.
6. **A `PeerLeave` that is not `DISCONNECTED` keeps appearance and equipment**,
   as `player:exit` does today.
7. **Mailboxes are bounded and latest-wins.** A slow client gets coarser
   updates, never unbounded server memory. Checkpoints, appearance, and
   equipment are latest-wins per peer ([gap 3](#gaps-resolved-before-execution)).
   Ordered messages (enter, leave, context, actions, chat, results) are never
   dropped. A client whose ordered backlog exceeds `limits.ordered_backlog`
   (default 4096 messages) is disconnected and reconnects cleanly.
8. **Link-dependent limits are configuration**: `send_rate_hz`,
   `uplink_bytes_per_second`, `interest.enter_radius`, `interest.leave_radius`,
   `interest.min_dwell_seconds`, `interest.update_hz`, `interest.max_visible`,
   `budget.enabled`, `budget.per_recipient_bytes_per_second`.
9. **Durations use seconds**, as in the plugin's `AGENTS.md` timing rules, and
   an injectable clock so hub tests are deterministic.

## Gaps resolved before execution

Found by checking the plan against the code on 2026-09-30. Each resolution is
part of the phase it names.

1. **Liveness while the game is paused.** `ConnectionMonitor`'s timers
   accumulate the frame delta, which is zero while paused and in the main menu,
   so a game-driven `Ping` stops and the server would close a paused player
   after 10 s. Today Engine.IO pings run on the transport thread. Resolution
   (phase 1): the server sends a WebSocket ping every 5 s and closes a
   connection whose pong or message has not arrived for 10 s; the client's
   library answers pings on its own thread. `Ping` stays the game-driven latency
   probe, and a missing `Result` for 5 s of frame time still abandons the
   connection on the client.
2. **Ordering between latest-wins checkpoints and ordered messages about the
   same peer.** Invariant 3 covered actions only, but a `PeerContext` or
   `PeerLeave` can also be overtaken by, or overtake, a pending checkpoint in a
   recipient's mailbox. Resolution (phase 1): one outbox per recipient. Before
   an ordered message about peer P is appended, P's pending checkpoint in that
   outbox is moved into the ordered stream in front of it; a `PeerLeave`
   discards it instead. Invariant 3 is this rule plus the forced anchor.
3. **Ordered backlog as a denial of service.** Appearance and equipment were
   listed as never-dropped ordered messages, so one client republishing them
   quickly could push every recipient past the ordered backlog limit.
   Resolution (phase 1): they are latest-wins per (recipient, peer): an unsent
   one is replaced in place, keeping its position. Actions are limited per
   sender (`limits.actions_per_second`, default 10, burst 10); an action over
   the limit is rejected. The ordered backlog limit then only trips on a
   client that does not read.
4. **Send rate versus the latency goal.** Today's tick is 40 Hz; 20 Hz adds up
   to 25 ms of batching wait and breaks "latency no worse than today".
   Resolution: `send_rate_hz` defaults to 40 in both profiles. A recipient gets
   a batch only on a tick with something for it. The homelab can set 20 to
   halve per-packet overhead (about 1.1 KB/s per client) at that latency cost;
   phase 4 measures which is better under the budget.
5. **Loadgen latency across processes.** The wire has no timestamp. Resolution
   (phase 1): loadgen bots use the send time in microseconds since the Unix
   epoch as `sequence`, which is strictly rising and legal, and receivers
   subtract it from their receive time. No wire field is added.
6. **Phase 1 wire.** Phase 1 already uses `PeerEnter`, `PeerLeave`
   (`EXITED`, `DISCONNECTED`), `PeerContext`, and slots, with a single rule:
   every connected client subscribes to every placed peer. The plugin's receive
   side then changes once, and phase 2 only adds rules on the server.
7. **Placement.** A peer is a candidate only after `Enter`, a `Context`, and a
   `Checkpoint`. A `Checkpoint` without a context since the last `Enter` is
   rejected (`no context`). From phase 2, a viewer without a placement sees
   nobody, and its subscriptions end with `LEAVE_REASON_VIEWER_UNPLACED`.
8. **Rank churn at the cap.** With `max_visible`, peers near the cutoff would
   swap every recomputation. Resolution (phase 3): an incumbent keeps its place
   unless a candidate is closer by `interest.cap_hysteresis` (default 1024
   units), and the minimum time in view applies to cap evictions too.
9. **Boundary limits.** Resolution (phase 1): read limit 16 KiB per message
   (the largest valid `ClientMessage` is about 12.9 KB); at most
   `limits.messages_per_second` (default 120, burst 240) per connection, over
   which the connection is closed; a `Hello` must be the first message and
   arrive within 5 s.
10. **Protocol mismatch.** Resolution (phase 1): the server closes the
    connection with status 4001 and reason `protocol <server version>`. The
    client shows it in the status panel and does not retry until the endpoint
    changes or the player presses Connect.
11. **Load-test transports and the CLI replay.** Resolution (phase 1):
    in-server batches (virtual bots through the hub input) stay in the admin
    panel. The `socket` transport and `uv run replay` are replaced by
    `cmd/loadgen`, which runs real WebSocket clients out of process.
12. **TLS for the remote server.** Resolution: the Go server speaks plain
    HTTP and WebSocket; the remote deployment terminates TLS in a reverse proxy
    (for example Caddy), and the client maps `https://` to `wss://`.
13. **nanopb sizes.** Static sizes and counts come from
    `proto/skyrimmp/v1/*.options`, which mirror the validation limits.
    `PeerStates.states` is decoded with a callback, entry by entry. The
    decoder's message is allocated once per connection, not on the stack
    (`ServerMessage` is about 21 KB).
14. **Unmeasured phase 1 load.** The prototypes measured filtered delivery
    only. Sending everything at 250 players is about 1.25M entries/s, but as
    concatenated copies into 250 × 40 writes/s. Phase 1's loadgen check measures
    it rather than assuming it.
15. **Rate classes and the client's interpolation.** `ActorMovement` chases
    the latest target at the reported speed with a backlog drain, so a far peer
    at a low rate may reach its target and wait. Phase 3's in-game check covers
    it; if it stutters, the client extrapolates by the measured interval.

## Phases

Each phase ends with its specs updated and the checks listed. The plugin cannot
be built on Linux: every plugin change needs the Windows xmake build and an
in-game check by the user.

### Phase 0: preparation

Done 2026-09-30: doc mismatches fixed, schema in `proto/skyrimmp/v1/` with
nanopb options, `proto/generate.sh` (`scripts/` is gitignored for local tools), and the decisions above.

- Fix the doc mismatches listed [above](#premises-corrected-by-the-code-review).
- Write `proto/skyrimmp/v1/*.proto` from the [draft](#wire-protocol-draft) and
  review field numbers and validation rules.
- Decide:
  - the Go server location: `server-go/` during the migration, or directly in
    `server/`
  - the plugin WebSocket library: IXWebSocket or websocketpp. Both are in
    xmake-repo; TLS on Windows must be checked
  - whether the admin feed stays a WebSocket or becomes server-sent events
- Add `proto/generate.sh`, which regenerates the Go and nanopb code, so nanopb's
  Python generator never runs under wine.

### Phase 1: Go server and protobuf transport, still sending to everyone

Goal: change the transport and codec with no change in behavior. Every
connected client in the world receives every peer, as today, but in protobuf.
On the homelab this moves the ceiling from about 26 to about 114 players.

Server:

- `session`, `validate`, `world`, and `delivery` with one class, "everyone".
- Encode-once entries and pooled per-recipient batch buffers.
- Send rate from configuration: 20 Hz on the homelab, 40 Hz on 1 Gbit/s.
- Everything the Python server has, so it can be retired at the end of the
  phase (the plugin cannot talk to both):
  - late-join state
  - name index
  - chat, including the rate limit
  - recording format v3: JSONL with protojson payloads of client messages
  - `convert-recordings` for v2 files
  - playback virtual players through the same hub input as connections
  - the admin REST API
  - the admin feed. `server/admin-ui/src/api/socket.ts` is the only socket.io
    user in the UI
- `cmd/loadgen`. Because loadgen clients share a clock, they measure latency
  from sender to receiver directly.

Plugin:

- Replace `socket-io-client` in `xmake.lua` with the chosen WebSocket library
  and nanopb.
- Replace `network/SocketIOClient` with a client and codec that decodes and
  validates on the transport thread and queues plain values as today.
- Keep `ConnectionMonitor`'s policy. The socket.io-specific parts go away:
  the 3 s `sync_close` wait and handler re-registration.
- Quantize in `src/protocol/`, where both directions share the rules.
  `CheckpointPublisher` sends `Checkpoint` and `Context`.
- Key peers by slot in `SyncCoordinator`.
- Make `TrafficMeter` count exact bytes. `PacketSize` estimation goes away.

Checks:

- Go unit tests: validation table, hub ordering, playback.
- `go test` black-box tests over a real WebSocket.
- loadgen at 250 bots sending everything, with the 1 Gbit profile: server CPU
  under 1 core, p99 under 60 ms.
- In game (user): movement, attacks, furniture, mounts, death, appearance, and
  equipment unchanged against a replayed player and a second client. Check that
  quantization causes no visible jitter in position or heading.

### Phase 2: interest and exclusions

- `interest` with the rule interface, the [first exclusion rules](#rule-interface),
  distance with separate enter and leave radii (defaults 8192 enter, 9830 leave,
  1 s minimum in view), and recomputation at 5 Hz plus on events.
- `PeerEnter`, `PeerLeave`, `PeerContext`; appearance, equipment, and actions
  go to subscribers only. All of [Invariants](#invariants) 1–6.
- Plugin: handle `PeerEnter` and `PeerLeave`. Every reason removes the actor
  and forgets world state as `player:exit` does; `DISCONNECTED` also forgets
  presentation.
- Admin panel: per-player visible count, bytes per second, drops, hidden and
  excluded state and controls.

Checks:

- Hub tests with a fake clock:
  - enter and leave hysteresis
  - exclusion taking effect at once
  - anchor forcing before an action
  - enter order
  - a paused sender seen by a newcomer
  - appearance revision tracking across leave and re-enter
- loadgen scenarios: sparse, dense city (250 within one radius), and churn
  (bots walking in and out of range), measured in both link profiles.
- In game:
  - a peer walks out of range and back: despawn and respawn, and attacks work
    after re-entering
  - an excluded peer disappears at once
  - no flapping at the edge of the radius

### Phase 3: caps, idle suppression, view radius

- `interest.max_visible`, nearest first. The default comes from measuring
  client frame cost in game with many remote actors, not from bandwidth.
- Idle suppression: the hub skips an entry whose quantized bytes, apart from
  `sequence`, equal the last sent entry. Invariant 3 still forces the anchors
  actions need.
- `Hello` carries the client's view radius, clamped by the server; add a
  `[Network]` key to `configuration.md`.
- Per-pair rate classes by distance (for example 20 Hz near, lower far),
  configurable.

Checks:

- Egress at 250 dense bots drops about 45% with idle suppression, as the
  recordings predict.
- In game: an idle player stays correct, attacks from standing still are not
  delayed, and far peers still move smoothly at the lower rate.

### Phase 4: budget scheduler

Required on the homelab at 250 players; a safety net on 1 Gbit/s.

- Global egress token bucket at `uplink_bytes_per_second` × 0.8. The router's
  upload buffer must never be what limits the rate, or latency rises for
  everyone.
- Per-recipient budget: the global budget shared by active recipients, capped
  by `budget.per_recipient_bytes_per_second`, with unused share passed on.
- Priority accumulator: each tick, the changed visible senders are ranked by
  waiting time × weight (distance, later party). Entries are taken until the
  tick's byte budget is spent; the rest keep their accumulated priority.
- Ordered messages are outside the budget, with their own rate cap.
  Appearance bursts on entering a crowd go nearest first and are spread over
  several ticks.

Checks: loadgen with a 50 Mbit/s profile at 250 dense bots keeps egress within
budget, p99 within the target for the nearest peers, and no starvation (every
visible peer updates at least once per second).

### Phase 5: later and optional

- Split the hub by space once CPU needs it, at about 1500–2000 players.
- UDP or QUIC for checkpoints, with a reliable channel for everything else, if
  real internet play shows TCP head-of-line stutter under packet loss. The
  header saving is only about 4% of the budget.
- Proximity or channel chat through the same rules.
- Field subsets for far peers, using per-variant entry caches so each entry
  stays encoded once.
- Revisit Rust only under the conditions in [Go vs Rust](#go-vs-rust).

## Risks

| Risk | Mitigation |
| --- | --- |
| Actors freeze when a peer's updates stop without an explicit leave | `PeerLeave` for every reason; invariant 1 |
| Attacks stop after a filtered gap | Invariant 4 |
| Attacks delayed by suppressed anchors | Invariant 3 |
| Spawn and despawn flapping at the edge; spawn cost | Separate enter and leave radii, minimum time in view, cap, `SpawnsPerSecond` |
| Load-order mismatch breaks visibility, because the server compares raw `cellId` and `worldspaceId` | Same limitation as the client today. Later: plugin-qualified space IDs, like furniture |
| Radius larger than what the client can spawn (loaded cells only) | Default 8192, client-reported radius clamped by the server |
| Uplink saturation fills the router's buffer | Global budget below link rate (phase 4) |
| Big-bang cutover: the plugin cannot talk to the Python server after phase 1 | Phase 1 has no behavior change and full tooling parity. Tag the last Python-compatible commit |
| Quantization artifacts | Verify in game in phase 1; finer position scale as fallback |
| Garbage-collector pressure in Go | Pooled buffers, entries encoded once; profile with `pprof` under loadgen |
| Replay and bots bypassing delivery (today `SocketPublisher` emits directly) | Playback uses the same hub input as connections |

## Specs to update

- `specs/network.md`: wire messages and validation; client and server
  components; connection and latency sections rewritten for WebSocket and
  protobuf.
- New system spec `specs/interest-and-delivery.md`: interest, rules, delivery,
  budget, invariants.
- `specs/sync-pipeline.md`: receive state keyed by slot, `PeerEnter` and
  `PeerLeave` handling, context separate from checkpoint.
- `specs/recording-and-replay.md`: format v3, converter, loadgen, admin feed.
- `specs/configuration.md`: `ServerEndpoint` scheme mapping, view radius key.
- `specs/ui-overlay.md`: traffic counted exactly.
- `specs/README.md`: new system and feature rows.
- `AGENTS.md`: Go server layout and test commands (`go test ./...`) in place of
  `uv run ...`, and the protobuf regeneration step.
- `server/README.md`: rewritten for the Go server.

## Reproducing the benchmarks

The prototypes and scripts from 2026-09-29 were temporary and are not in the
repo. Phase 1's `cmd/loadgen` replaces them. To compare against these numbers,
keep the same shape:

- 20 Hz clients in separate processes on the same host.
- Clients spread uniformly over ±100000 units (sparse) or ±20000 units (dense),
  with an 8192-unit radius.
- Report server CPU from `/proc/<pid>/stat`, egress bytes per second, and p50,
  p99, and max latency.
- Use port 3100, because a local development server usually holds 3000.
