# Network

## Status

- Server: implemented in Go. `go test -race ./...` in `server/` covers
  validation (`internal/validate`), hub ordering and relay (`internal/hub`),
  outboxes (`internal/delivery`), and black-box sessions over a real WebSocket
  (`internal/session`, `internal/admin`), including the `Origin` rule. A
  local server answered `/ws` and `/health` only on `listen`, `/api`,
  `/admin/`, and `/debug/pprof/` only on `admin_listen`, and refused a
  WebSocket upgrade with an `https://` origin while accepting `ws://` and
  none. `cmd/loadgen` measured 250 walking
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
  display name requirement is not built on Windows either.
- **Sessions:** not built on Windows or run in game. Server: `go test -race`
  covers the store (`internal/auth`: issue, resume, unique display names, a
  resumed session renamed by its hello, sliding expiry, a reopened database, a
  version 1 database upgraded), the hub (the session's display name on the
  appearance, a session connecting again), and black-box connections
  (`internal/session`: issued and resumed sessions, a hello renaming a resumed
  session, every refusal, the earlier connection closed with 4006, an unused
  new session deleted and a resumed one kept, a full server refusing a new
  session but admitting a connected one, the per-address allowance by peer
  and by forwarded address, and the pending hello cap); `internal/admission`
  covers the gate and `X-Forwarded-For` parsing; `internal/admin` covers
  `POST /api/sessions` issuing past the allowance, resuming, a hello resuming
  an issued session, and its 409 and 422 answers. The plugin's 4007 and 4008
  refusals are not compiled or run. `cmd/loadgen` ran 100 bots
  twice against one database on port 3100 without a refusal, and 20 bots
  against an in-memory store with protocol version 4. Plugin: `ServerClient`,
  `WireCodec`, `SessionStore`, and `ConnectionMonitor` were built on Linux
  against IXWebSocket 12.0.1 and nanopb 0.4.9 with a stubbed configuration and
  game state, and run against that server: a session issued and saved to
  `playerhub.session`, resumed by a second process from the file, a hello with
  a held display name refused without retrying, and a second process with the
  same token closing the first, which stayed down. That run was before the wire
  fields were named `display_name`, before the display name could change only
  with a hello, and before a refusal ended the player's choice to connect.
  Since then `ServerClient.cpp`, `WireCodec.cpp` with the regenerated nanopb
  code, and `ConnectionMonitor.cpp` passed a syntax check, and
  `ConnectionMonitor.cpp` ran on Linux under the settings window against a
  stubbed transport ([UI overlay](ui-overlay.md#status)): a game start with
  and without a display name and `AutoConnect`, a refusal for the display
  name, the protocol version, and a replaced session, each leaving the player
  disconnected without a retry until Connect, and a lost connection reopened.
  The display name rule is covered by `internal/validate` and
  `internal/session` tests; the client's `Multiplayer::IsValidDisplayName` was
  checked by compile-time assertions on Linux, and `WireCodec.cpp` passed a
  syntax check with the regenerated nanopb code.
  `ChatSync.cpp` passed a syntax check; `AppearanceSync.cpp` was not compiled.
  The UI that shows refusals is in [UI overlay](ui-overlay.md#status).
- **Roster and whispers by slot:** not built on Windows or run in game.
  Server: `go test -race ./...` covers the roster in the hub (the list after
  a welcome, a join and a leave reaching every other client once, replays
  left out, a session connecting again) and over a WebSocket
  (`internal/session`), whispers by slot (to another client, to oneself, to
  an unknown slot, a replay, and the slot of a client that left), and the
  recipient rule of `validate.Chat`. `cmd/loadgen` connected 500 bots within
  2 s to a server on port 3100 and ran 15 s without a failed bot or a server
  warning. Plugin: `WireCodec.cpp` with the regenerated nanopb code was built
  on Linux and decoded Go-encoded rosters (3 and 500 entries, a leave, both
  at once, none) and a whisper, and rejected entries without a slot, without
  a name, with a control character, and with a 129-byte name; its `ChatSend`
  for a whisper and for a local line passed the server's `validate.Chat`, and
  one with a recipient on another channel or a whisper without one did not.
  The roster kept in `ServerClient.cpp` passed a syntax check only.
- **Session IDs to peers and vanish:** not built on Windows or run in game.
  Server: `go test -race ./...` covers the session ID on roster entries and
  chat speakers, a vanish that takes effect at once and one way, that holds
  when the vanished session connects again under another slot, that is lifted
  at once, a hello's vanished sessions applied before the client is placed,
  the 1024 limit (`internal/hub`), the hello and `Vanish` rules
  (`internal/validate`), and over a WebSocket a hello that vanishes a player
  and one refused with `invalid vanish` (`internal/session`). `cmd/loadgen`
  with 250 bots within ±3000 units on port 3100 measured the same server CPU
  (36–37%) and latency (p50 30 ms, p99 52 ms) as the server before. Plugin:
  `WireCodec.cpp` with the regenerated nanopb code was built on Linux; its
  hello with three vanished sessions and its `Vanish` decoded in Go and passed
  validation, it encodes no hello with 1025, and it decoded a Go-encoded
  roster and whisper with session IDs and rejected both without one.
  `PlayerFilterStore.cpp` ran on Linux under the chat harness
  ([UI overlay](ui-overlay.md#status)). The vanish handling in
  `ServerClient.cpp` (the hello's list, a `Vanish` held back until the hello
  went to the same server) was not compiled.
- **Inspection:** not built on Windows or run in game. Server:
  `go test -race ./...` covers the reply for a client with an appearance,
  for the asking client itself, for a replay (not listed), and for an unknown
  slot, that nobody else receives it, the rate limit and its refill
  (`internal/hub`), and the zero slot rule (`internal/validate`). Plugin:
  `WireCodec.cpp` with the regenerated nanopb code was built on Linux and
  decoded Go-encoded inspections (full, slot only) and rejected one without a
  slot, one with a level over 65535, and one whose default race is not from a
  base game plugin; its `Inspect` for slot 7 encodes as the server's schema
  reads it. The queue in `ServerClient.cpp` was not compiled.
- **Official and custom server:** not built on Windows or run in game.
  `ConnectionMonitor.cpp` ran on Linux under the settings window's harness
  with the real configuration and a stubbed transport
  ([UI overlay](ui-overlay.md#status)): a game start connects to the official
  server; switching `ConnectToOfficialServer` off with a URL and on again
  reconnects to each at once; switching it off without a URL closes the connection, logs why,
  and stays disconnected when a URL is then entered until Connect; a game
  starting without an endpoint does not connect, and switching off then waits
  for Connect. `ServerClient.cpp`, `SessionStore.cpp`, and `PlayerFilters.cpp`
  passed a syntax check only, so `DescribeServer` and `GetServerAddress` did
  not run.

## Scope

This system owns the wire protocol, the client transport and connection
management, and the server's connections, player [sessions](#sessions),
validation, and per-player state. It
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
| `proto/playerhub/v1/` | The schema (`state.proto`, `messages.proto`) and nanopb size options. `proto/generate.sh` regenerates `server/internal/wire/pb` and `src/network/proto` |
| `src/protocol/` | Plain wire values and shared validation: `PlayerState.h`, `MovementState.h`, `AppearanceState.h`, `EquipmentState.h`, `FormReference.h`, `ChatMessage.h`, `PeerSlot.h`, `Session.h`, and the quantization rules in `Quantization.h`. No Skyrim headers |
| `network/WireCodec` | nanopb encoding of client messages and validated decoding of server messages |
| `network/ServerClient` | IXWebSocket transport: sends, dispatches received messages, merges each peer's context into its checkpoints, and queues per-player changes for `DrainRemotePlayerChanges` |
| `network/ConnectionMonitor` | When to connect, reconnect schedule, latency probes, refusal, and the connection state |
| `network/SessionStore` | The session tokens in `playerhub.session`, one per server |
| `network/Refusal.h` | Why the server refused a connection, for the monitor and the UI |
| `network/TrafficMeter` | Bytes per second in each direction |
| `server/internal/session` | WebSocket accept, hello and session authentication, reader and writer goroutines, liveness, per-connection limits |
| `server/internal/auth` | The session store: tokens, unique display names, expiry |
| `server/internal/admission` | Caps on players, pending hellos, and new sessions per address; the client address behind trusted proxies |
| `server/internal/validate` | Field validation of client messages |
| `server/internal/world` | A player's accepted state and the ordering rules |
| `server/internal/wire` | Hand-written encoding of checkpoint entries and batches, quantization for tools |
| `server/internal/hub` | Applies accepted messages on one goroutine ([Interest and delivery](interest-and-delivery.md)) |

## Transport

- One binary WebSocket message carries one `ClientMessage` or `ServerMessage`,
  each a protobuf `oneof`. The server listens at `/ws` on `listen`; the admin
  panel and API are served only on `admin_listen`
  ([Recording and replay](recording-and-replay.md#admin-api)).
- The server refuses with 403 an upgrade whose `Origin` scheme is not `ws` or
  `wss` (`session.playerOrigin`), so a web page cannot make its visitors'
  browsers join: a browser always sends the page's origin, which is never
  `ws` or `wss`. IXWebSocket sends `Origin: ws://host:port` or
  `wss://host:port`, and Go clients such as `cmd/loadgen` send none, so both
  join. A bare reject of any `Origin` would refuse the plugin. The origin's
  host is not compared with `Host`, since behind a proxy the plugin names the
  public host and port.
- The endpoint (`Config::GetServerEndpoint`: the official server, or
  `[Network] ServerEndpoint` with `ConnectToOfficialServer` off,
  [Configuration](configuration.md#official-and-custom-server)) keeps its
  `http://` or `https://` form; the client maps it to `ws://` or `wss://` and
  appends `/ws` when the endpoint has no path. TLS is terminated by a reverse proxy in front of the server.
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
- The first client message must be
  `Hello{protocol_version, view_radius, session_token, display_name, vanished}`
  within `limits.hello_timeout_seconds` (5). A version other than
  `Protocol.PROTOCOL_VERSION` (7) is closed with status 4001 and reason
  `protocol <server version>`; anything else first is closed with 1008. The
  session fields are in [Sessions](#sessions).
- The server answers a hello with
  `Welcome{slot, session_id, display_name, session_token}`. Slots are `uint32`
  from a counter that only rises and are never reused within a server run, so a
  slot names one connection. Replays and in-server bots get slots too.
- Liveness runs on the transport, not the game: the server pings every
  `liveness.ping_interval_seconds` (5) and closes a connection whose pong does
  not arrive within `liveness.timeout_seconds` (10). IXWebSocket answers pings
  on its own thread, so a paused game stays connected.
- Boundary limits: 16 KiB per message (the largest valid `ClientMessage` is
  about 12.9 KB); `limits.messages_per_second` (120, burst 240) per connection,
  over which the connection is closed with 4003; a recipient whose unsent
  ordered messages exceed `limits.ordered_backlog` (4096) is closed with 4002.
  Compression is off on both ends.

| Close status | Meaning | Client |
| --- | --- | --- |
| 4001 | Another protocol version | refusal |
| 4002 | Ordered backlog overflowed | reconnects |
| 4003 | Message rate exceeded | reconnects |
| 4004 `display name taken` | Another session holds the hello's display name | refusal |
| 4005 `invalid display name` | The hello's display name is missing or not a display name | refusal |
| 4006 `session connected elsewhere` | The session connected again | refusal |
| 4007 `server full` | `limits.max_players` sessions are connected | refusal |
| 4008 `too many new sessions` | The hello needs a new session and its address took its allowance | refusal |
| 1008 | First message is not a hello, or `invalid session token` | reconnects |
| 1011 `session store unavailable` | The session store failed | reconnects |

An upgrade refused before the WebSocket exists answers HTTP 403 for a browser
([Transport](#transport)) or 503 while too many connections wait for their
hello ([Admission](#admission)); the client treats both as a failed attempt and
retries. What a refusal means for the client is in [Connection](#connection).

## Client messages

| Message | Handling |
| --- | --- |
| `Hello` | Handshake, above. `view_radius` is the client's `[Performance] ViewRadius` in game units, 0 for the server's ([Interest and delivery](interest-and-delivery.md#subscriptions)). `session_token` and `display_name` resume or issue the [session](#sessions). `vanished` lists the sessions the player [vanished](#vanish) on this server |
| `Enter`, `Exit` | World presence. `Exit` when not in the world does nothing |
| `Context` | A checkpoint's space and graph epoch ([below](#checkpoint-and-context)) |
| `Checkpoint` | One quantized sample |
| `Action` | One attack ([Actions](#actions)) |
| `Appearance`, `Equipment` | Latest-wins presentation ([below](#appearance-and-equipment)) |
| `ChatSend` | `{request_id, text, channel, recipient}`, answered with a `Result` ([Chat](#chat)) |
| `Ping` | `{request_id}`, answered with `Result{ok}` by the connection at once; the latency probe |
| `Vanish` | `{session_id, vanished}`: stop or resume receiving a session's player ([Vanish](#vanish)) |
| `Inspect` | `{slot}`: ask for a player's level and race, answered with an `Inspection` ([Inspection](#inspection)) |

A rejected message is dropped and logged on the server (at most one summary
per connection every 5 s); only `ChatSend` and `Ping` get a `Result`, and an
accepted `Inspect` gets an `Inspection`. Graph
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
| `name` | at most 128 UTF-8 bytes, no control characters (below U+0020, U+007F). The server replaces a client's with its [session's display name](#sessions) on the stored and relayed copy, and the client sends none; a replayed player keeps its recorded name |
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
create dynamic color forms.

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
| `Welcome` | `slot`, `session_id`, `display_name`, and `session_token` only when the session was issued for this connection ([Sessions](#sessions)) |
| `Result` | `request_id`, `ok`, `error` |
| `PeerStates` | `repeated Checkpoint` with `player` set; the concatenation of entries encoded once per sender |
| `PeerEnter` | `slot`, `appearance` and `equipment` when newer than what the recipient holds, `context`, `state` (the stored checkpoint) |
| `PeerContext` | `slot`, `context` |
| `PeerLeave` | `slot`, `reason`: `OUT_OF_RANGE`, `EXCLUDED`, `EXITED`, `DISCONNECTED`, `VIEWER_UNPLACED` |
| `PeerAction`, `PeerAppearance`, `PeerEquipment` | `slot` and the message |
| `ChatMessage` | `channel`, `sender`, `recipient` (whisper only), `text`; a `ChatSpeaker` is `slot`, `name`, `level`, `session_id` |
| `Roster` | `joined`, a list of `RosterEntry{slot, name, session_id}`, and `left`, a slot or 0 ([Roster](#roster)) |
| `Inspection` | `slot`, `level`, `race`, `default_race` of the player an `Inspect` named ([Inspection](#inspection)) |

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
  applied in arrival order by the hub. `Ping` is answered by the connection
  itself.

### Sessions

Goal: every connection belongs to a player the server can recognize again. A
session is that identity on one server: a public ID that later interactions
between players can refer to whatever the player is called, a secret only the
player's client holds, and a display name no other session has. Non-goals:
accounts, passwords, recovering a lost session, roles, bans, and protecting
the admin API.

| Part | |
| --- | --- |
| `session_id` | `uint64` from 1, the row ID of an `AUTOINCREMENT` column, so it is never reused, also after the session is gone. Sent to its own client in `Welcome`, and to every client on roster entries and chat speakers, which is how a client mutes or vanishes a player whatever slot or name it connects with. The hub keeps it on the player (`world.Player.Session`) |
| Token | 32 bytes from `crypto/rand`. The store keeps only its SHA-256, so a copy of the database resumes nobody. It is returned once, in the `Welcome` of the connection it was issued for |
| Display name | 3..15 (`validate.MinimumDisplayNameBytes`, `validate.MaximumDisplayNameBytes`) ASCII letters, digits, and underscores (`validate.DisplayName`; the client's `Multiplayer::IsValidDisplayName` in `src/protocol/Session.h` is the same rule). Without spaces, a name typed before a whisper's message is told from the message. `Hello.display_name` and `Welcome.display_name` are nanopb buffers of 16 bytes. Unique among sessions by `validate.DisplayNameKey`, its lower case; a session may respell its own |
| Last use | Set by every hello, when a connection closes, and hourly for connected sessions (`session.Server.Maintain`) |

- `auth.Store` keeps sessions in SQLite (`modernc.org/sqlite`, pure Go, pinned
  to the newest release that builds with Go 1.24) at `sessions.database`
  (`sessions.db`; empty keeps them in memory, for tests), with a write-ahead
  log and `user_version` 2. A version 1 database, whose columns are named
  `nickname` and `nickname_key`, has them renamed when opened and keeps its
  sessions. Its one connection serializes transactions, which
  the display name checks rely on. Store calls run on a connection's goroutine,
  never on the hub's.
- A session lives `sessions.lifetime_days` (30) after its last use, so one that
  keeps connecting never expires and none expires while connected. Expired
  sessions are deleted before every display name comparison and hourly, which
  frees their display names; their tokens then match nothing.
- The hello's `session_token` is empty or 32 bytes, else the connection is
  closed with 1008 `invalid session token`; a `display_name` that is not one is
  closed with 4005 `invalid display name`.
- A token of a live session resumes it. A display name other than the session's
  renames it first; one another session holds closes the connection with 4004
  `display name taken` and leaves the session as it was. The hello is the only
  message that sets a display name, so a session's name is fixed for as long
  as a connection lasts and changes only by connecting again.
- No token, or one that matches no live session (unknown or expired), issues a
  new session under the display name, or closes with 4004 when it is held. The
  client asks for a session by connecting; there is no separate request.
- A session issued for a connection that closes before the client sent anything
  after its hello is deleted (`Store.Abandon`). The welcome may not have reached
  the client, which then holds no token and would find its own display name
  taken until the session expired. A session whose token came back in a later
  hello is kept: its client holds the token.
- A session is connected once. When it connects again, `Hub.ConnectClient`
  disconnects the earlier connection's player first, so peers get its
  `PeerLeave` with `DISCONNECTED`, and the earlier socket is closed with 4006
  `session connected elsewhere`. A reconnect after a network loss therefore
  never waits for the liveness timeout of its dead connection. The takeover is
  decided on the hub goroutine, so two connections racing for one session end
  with one of them connected in the hub and open.
- A store failure closes the hello's connection with 1011
  `session store unavailable`.
- A new session is issued only when its address may take one
  ([Admission](#admission)); the check runs after the display name is found
  free, so a taken name does not spend the allowance.
- The admin API's `POST /api/sessions` `{displayName, token?}` runs the same
  resume-or-issue on `admin_listen`, without the allowance, for
  `cmd/loadgen` ([Load testing](recording-and-replay.md#load-testing)).
  `token` is base64 JSON, empty or 32 bytes, and `displayName` passes
  `validate.DisplayName`, else 422; a held display name answers 409. It
  answers `{sessionId, displayName, token}`, 201 with a new token or 200 with
  the resumed one. A session issued there that never connects is not deleted
  by `Store.Abandon`; it expires.
- A client is named by its session everywhere: `Appearance.name` on the stored
  and relayed copy, `ChatSpeaker.name`, the admin state, and a recording's
  header. Replays and in-server bots have no session and keep their recorded
  names, which are not compared with anyone's; a replay may show a connected
  player's display name.

### Admission

Goal: bound what connections cost before a player is admitted, so a flood of
connections or hellos cannot crowd out real players or squat display names.
`admission.Gate` decides; `session.Server` asks it. Admin replays and
in-server bots do not pass through it and do not count.

- `limits.max_pending_hellos` (64) caps connections between the upgrade and
  the end of their hello, which bounds the hellos queued on the store's one
  connection. Over it, the upgrade answers HTTP 503 before a WebSocket exists.
- `limits.max_players` (100; 0 is no cap) caps connected sessions. A hello
  without a token on a full server is closed with 4007 `server full` before
  the store is touched. Otherwise the session is admitted after it
  authenticates, unless the server is full and the session is not connected
  already: a session replacing its own connection is always admitted, so a
  reconnect never waits for its dead connection's timeout. A new session
  issued in the race between that check and admission is deleted again. The
  cap also bounds the roster every client receives.
- `limits.new_sessions_per_address_per_hour` (5; 0 is no limit) is a token
  bucket per address with that burst, refilled at that many per hour.
  Resuming a session never spends it, and neither does a session issued
  through the admin API ([Sessions](#sessions)). An IPv6 address counts with
  its /64, which one subscriber usually holds whole. `Server.Maintain` prunes
  refilled buckets hourly.
- The address is the TCP peer's, unless the peer is in `trusted_proxies`
  (addresses or CIDR prefixes, empty by default). Then it is the rightmost
  `X-Forwarded-For` hop not in `trusted_proxies`: each proxy appends the
  address it received from, so earlier hops may be forged by the client. A
  hop that does not parse stops the walk at the last trusted address. Behind
  a proxy without `trusted_proxies`, every player shares the proxy's
  allowance.
- Upgrades refused for the hello cap are logged as a throttled summary
  (`refused connections`), since a flood would repeat them.

### Chat

`ChatChannel` is `GLOBAL` (0), `LOCAL`, or `WHISPER`.

- The server trims surrounding spaces and rejects an unknown channel, empty
  text, control characters, more than 512 UTF-8 bytes, a `WHISPER` without a
  `recipient`, and a `recipient` on another channel
  (`invalid chat message`); protobuf already guarantees valid UTF-8.
- `GLOBAL` goes to every connected client.
- `LOCAL` needs a placed sender (`not in world` otherwise) and goes to every
  placed client in the same space: in a worldspace within `chat.local_radius`
  (8192, horizontal like interest), and in an interior to the whole cell. It
  ignores exclusions, hiding, and the visible cap, which govern presentation
  only. Recipients are found by a scan of connected clients, which the rate
  limit keeps cheap; the interest grid does not bound an interior.
- `WHISPER` names its recipient by slot, which the client takes from the
  [roster](#roster). A slot is one connection, so a whisper never reaches
  whoever holds the recipient's display name after it left, and names with
  spaces need no parsing on the server. A slot that is not a connected client
  is `unknown recipient`, which includes replays and bots, since they receive
  nothing. The whisper goes to the sender and the recipient (once when they
  are the same).
- 5 accepted messages per player in any 5 s over all channels
  (`rate limited`); rejected messages do not count, and a disconnect forgets
  the history.
- Every channel includes the sender, so everyone sees one order and the
  sender's own line confirms delivery. A `ChatSpeaker` is the player's
  display name, its session ID, and the level of its latest appearance (`0`
  before one); the client does not show the level
  ([Appearance and equipment](appearance-and-equipment.md#player-label)).
- The client rejects a `ChatMessage` without a sender, with an unknown channel,
  or with a recipient present exactly when the channel is not `WHISPER`, and
  validates speakers like appearance names, with a slot and a session ID, and
  text like `ChatSend`.
- Chat does not require being in the world except for `LOCAL`, keeps no
  history, and is not recorded. The client side is in
  [UI overlay](ui-overlay.md#chat).

### Roster

Every client knows who is connected, so it can complete a name, list the
players in chat ([UI overlay](ui-overlay.md#player-list)), and address a
whisper by slot. Proximity does not limit it: a whisper reaches any connected
player.

- The roster lists the clients that have a session, by slot, session display
  name, and session ID. Replays and in-server bots are not listed.
- `Hub.ConnectClient` sends the new client one `Roster` after its `Welcome`
  with every listed client in `joined`, itself included, and every other
  listed client a `Roster` with the new one in `joined`.
- A listed client that disconnects, also when its session connects again, is
  sent to every other listed client as `Roster{left}`. A display name changes
  only with a hello ([Sessions](#sessions)), so a rename is a leave and a
  join under another slot.
- Roster messages are ordered with the rest of a client's outbox
  (`Outbox.AppendGlobal`) and marshalled once per event. A join or a leave
  writes to every outbox; the full list is 5 to 11 bytes plus the name per
  client, sent once per connection.
- The client rejects a `Roster` whose entry has no slot, no session ID, no
  name, or an invalid name, or with more than `kMaximumRosterEntries` (4096)
  entries.

### Vanish

A client can stop receiving a player: its character leaves that client's
world and nothing of it is sent there. It is the server's half of vanishing a
player ([UI overlay](ui-overlay.md#mute-and-vanish)); the rule that applies it
is in [Interest and delivery](interest-and-delivery.md#subscriptions). Muting
is the client's alone and has no wire message.

- A client names a player by session ID, not slot, so the player stays
  vanished when it connects again; a session ID names one player on one
  server only.
- `Hello.vanished` holds at most `validate.MaximumVanished` (1024) session
  IDs, none zero; otherwise the connection is closed with 1008
  `invalid vanish`. `session.Server.serve` hands them to `Hub.SetVanished`
  with the view radius, before the client can be placed.
- `Vanish{session_id, vanished}` adds or removes one. A zero session is
  dropped as `invalid vanish`, one more than 1024 as
  `too many vanished players`, and a change to what is already so does
  nothing. Neither gets a `Result`, and an unknown session is accepted: the
  player may connect later.
- The set belongs to the connection (`interest.View.Vanished`), lasts until
  it closes, and is not recorded, since it is the receiver's choice rather
  than the player's presentation. The client presents its whole set again in
  every hello.
- The vanished player is not told and still receives the client, and chat is
  unaffected.

### Inspection

A player's menu shows the player's race and level
([UI overlay](ui-overlay.md#player-menu)), and a client holds them only for
the players it receives. The server holds every connected player's
appearance, so it answers for the others.

- `Inspect{slot}` with a zero slot is dropped as `invalid inspect`.
- `Hub.inspect` answers the asking connection alone, from its global queue,
  with `Inspection{slot, level, race, default_race}` copied from the named
  player's stored `Appearance`. A slot that is not a listed client
  ([Roster](#roster)), or one without an appearance yet, gets
  `Inspection{slot}` alone, so the client stops waiting. Asking about oneself
  is answered too.
- Each connection may ask twice a second with a burst of 5
  (`inspectsPerSecond`, `inspectBurst`, a `limit.TokenBucket`); one more is
  dropped as `rate limited` without a reply. A reply is small and goes only
  to the asker, so the limit guards the hub's work rather than anyone's
  bandwidth.
- Neither is recorded; vanish and hiding do not change the reply, since the
  name is already on the roster and the race and level show on the label of
  any character in range.

## Client

- `ServerClient` runs its callbacks on the IXWebSocket thread. `WireCodec`
  validates each message as the server does; a rejected one is logged with the
  offending field, and plain values are queued; nothing touches game objects.
  The nanopb `ServerMessage` buffer is allocated once per thread, and
  `PeerStates` is decoded entry by entry (at most 4096).
- Nothing before `Welcome` is accepted. `Welcome` advances the connection
  generation and clears all receive history. A `Welcome` without a slot, a
  session ID, or a valid display name (`Multiplayer::IsValidDisplayName`), or
  with a token of another length, is rejected.
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
- The transport keeps the roster: `Roster` is decoded entry by entry, like
  `PeerStates`, and applied under its own lock, a join replacing an entry
  with the same slot. A welcome and a disconnect empty it. Every change
  advances a revision, and `Network::CopyRosterIfChanged` copies the list,
  with the connection's own slot, only for a caller that holds an older one,
  so readers pay for a copy when a player joins or leaves and not per frame.
- `Network::CopyNearbyPlayers` lists the peers with a context, which are the
  ones the client currently receives; chat orders its name list by it.
- `Network::DescribeServer` gives `the official server` for the official
  endpoint in either form, and any other URL as it is. Every log line that
  names a server goes through it, so players can share a log without the
  official URL: a failed or abandoned connection, vanishes over the limit,
  saving a session, and muting or vanishing a player.
- `Network::RequestInspection` sends `Inspect` while connected and reports
  whether it did. An `Inspection` without a slot, with a level above 65535,
  with an invalid race, or with a default race that is invalid or not from a
  base game plugin is rejected, as an `Appearance` with them would be. The
  newest per slot is queued with the remote changes, drained as
  `RemotePlayerChanges::inspections`, and dropped with them when the
  connection is lost.

### Session

- `SessionStore` keeps one token per server in `playerhub.session`, next to
  `playerhub.ini` (`Config::GetDirectory`): a line of 64 hexadecimal digits, a
  space, and the WebSocket URL the connection is opened to; lines starting with
  `;` are comments. It is a file of its own because players share their ini and
  modpacks, and a token is all it takes to connect as its owner; one per server
  lets a player switch servers and back. The file is read once, on first use,
  and written through `playerhub.session.tmp`. Tokens are never logged.
- When the socket opens, the transport thread sends the hello with the token
  kept for the attempt's URL, or none, and `[General] DisplayName`.
- A `Welcome` with a token saves it, also when the connection is already being
  closed, since the session exists on the server either way. A failed save is
  logged as an error: the token lasts until the game exits.
- The welcome's session ID and display name are only logged; the roster
  carries the local player's session ID too.
- The client sends no `Appearance.name`: the hello carries the display name,
  which holds for the connection ([Connection](#connection)).

### Player filters

- `PlayerFilterStore` keeps the local player's friends and the players it
  muted or vanished in `playerhub.players`, next to `playerhub.session`: a
  line per player and server, any of `f`, `m`, and `v` in that order, the
  session ID, the WebSocket URL, and the name when last seen
  ([UI overlay](ui-overlay.md#friends)), separated by tabs, which neither a display
  name nor a URL holds; lines starting with `;` are comments, and a line
  without a session, a server, a name, or a known flag is skipped with a
  warning. It is read once, on first use, and written through
  `playerhub.players.tmp` on every change; a failed write lasts until the
  game exits. A player with none of the three has no line.
- The hello carries the sessions vanished on the attempt's URL, at most 1024
  with a warning for the rest. `Network::PublishVanish` sends a change only
  when the current socket's hello went to the server the change belongs to,
  under the lock the hello is sent under: a `Vanish` before the hello would
  end the connection, and the hello reads the file, which the caller updates
  before publishing, so a change made meanwhile is in one or the other.
- `Network::GetServerAddress` is the URL of the latest welcomed connection, or
  of the configured endpoint before the first, or empty without one: the
  server whose session IDs the roster and chat lines carry. `PlayerFilters`
  reads the file for it again whenever the connection generation changes
  ([UI overlay](ui-overlay.md#mute-and-vanish)), and changes nothing while it
  is empty.

### Latest-wins publishers

Every welcome advances the connection generation. Appearance and equipment
republish their last state on each new generation, because the server forgets
a connection's state when it drops; `PresenceSync` sends `Enter` again, and
the next checkpoint sends its context. Remote players are then rebuilt from
the server's `PeerEnter` messages.

### Connection

A connection is wanted only in a game, while the player's choice enables it,
once `[General] DisplayName` is set
([Appearance and equipment](appearance-and-equipment.md#display-name)), and
while `Config::GetServerEndpoint` is not empty, which it is only with
`ConnectToOfficialServer` off and no `ServerEndpoint`
([Configuration](configuration.md#official-and-custom-server)).
`ConnectionMonitor` owns that decision; nothing else opens or closes the
client.

- At `kDataLoaded`, `Network::Initialize` only initializes the socket library;
  the main menu never connects.
- A game starts when `PresenceSync` first finds the player in the world, and
  ends when the main menu opens (`GameSession`). Loading screens keep the game
  and its connection.
- At a game start the choice becomes `[Network] AutoConnect` (default `true`)
  if a display name and an endpoint are set, and disabled otherwise. The settings window's
  Connect, Cancel, and Disconnect change it with
  `Network::SetConnectionEnabled` until the game ends.
- Applying a display name never opens a connection; only the choice does. The
  monitor reads the display name when the configuration revision changes, and
  one that becomes empty disables the choice, so entering one again waits for
  Connect. The server requires one too, in the hello. An endpoint that
  becomes empty does the same, and also closes the connection.
- A connection keeps the display name its hello carried. The settings window
  locks the field while a connection is wanted or open
  ([UI overlay](ui-overlay.md#connection-controls)), so the configured display
  name does not change under one, and the monitor does not compare them.
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
- An endpoint that differs from the latest attempt's, after editing
  `ServerEndpoint` or switching `ConnectToOfficialServer`, abandons a live
  connection and reconnects at once, with the attempt count reset.
- A close with status 4001, 4004, 4005, 4006, 4007, or 4008 is a refusal:
  retrying at once cannot help, so the monitor disables the player's choice and closes the client,
  which leaves `kDisconnected`. `Network::GetConnectionRefusal` returns its
  `Refusal{reason, message}` for the settings window's [connection
  controls](ui-overlay.md#connection-controls) until the player presses
  Connect or a game starts or ends. Nothing retries by itself, also not an
  applied display name or endpoint: the player changes what the refusal names
  and presses Connect. 4006 is a refusal because retrying would close the
  connection that replaced this one, and two games holding one token would
  take the session from each other without end.

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
3. A client with another protocol version shows the server's version in the
   settings window and stays disconnected until Connect.
4. A paused game (menu open) stays connected for minutes.
5. Quantization shows no visible jitter in position or heading.
6. A client without a display name opens no connection, whatever `AutoConnect`
   says, and cannot press Connect; applying one does not connect until Connect
   is pressed.
7. A first connection to a server writes its line to `playerhub.session`; later
   games, and reconnects after a loss or a server restart, resume the same
   session.
8. Two players never show the same display name, compared without case, also
   while one of them is offline; a session unused for 30 days frees its display
   name.
9. Connecting with a display name another session holds shows in the settings
   window that another player has it and leaves the player disconnected;
   applying a free display name and pressing Connect connects.
10. The display name cannot be edited while a connection is wanted or open.
    After Disconnect, applying another one and pressing Connect resumes the
    same session under it, and peers see the player leave and enter with the
    new display name.
11. A second game started with a copy of `playerhub.session` takes the session;
    the first shows that its session connected from another game and stays
    down until Connect.
12. A reconnect after a network loss is welcomed at once, without waiting for
    the server to time out the dead connection.
13. On a server with `max_players` connected players, a new player sees that
    the server is full and stays disconnected until Connect, while a connected
    player whose network dropped reconnects.
14. A web page cannot open a player connection, and players behind a
    configured reverse proxy are limited by their own address, not the
    proxy's.

## Known limitations

- Sessions authenticate players only. The admin API can delete recordings and
  spawn virtual players without one; it is served only on `admin_listen`,
  which must not face the Internet.
- The `Origin` rule stops browsers only. Any other program can open
  connections and send hellos from its own address.
- A session cannot be recovered: a player who loses `playerhub.session`, or
  whose plugin could not write it, gets a new session, and the old one keeps
  its display name until it expires.
- Display names can still be squatted from many addresses, slowly from one
  (`limits.new_sessions_per_address_per_hour`), or by a program, which the
  `Origin` rule does not stop. Resumed hellos are not limited per address.
- A full server or an exhausted allowance leaves the player disconnected until
  Connect; nothing retries when a place frees.
- Display names compare by lower case only, so look-alikes such as `l` and
  `I` or `0` and `O` are different display names.
- Non-goal: display names in other scripts, with spaces, or with punctuation
  besides `_`. The narrow rule keeps names typeable in a whisper, renderable by
  every font, and comparable without Unicode case folding. Sessions stored
  before the rule keep their names until they expire or a hello renames them.
- The token is sent in the hello as it is, so an `http://` endpoint exposes it
  to the network; `https://` endpoints carry it under TLS.
- The official URL is kept out of the settings window and the log only: it
  is a plain string in the DLL, the key of its lines in `playerhub.session`
  and `playerhub.players`, and IXWebSocket's reason for a failed connection
  is logged as the library gives it, which may name the host.
- Admin exclusions and hiding are still keyed by slot
  ([Interest and delivery](interest-and-delivery.md#admin-controls)); only a
  player's own vanish follows the session.
- Chat has no history for late joiners.
- The roster tells every client the display name of every connected player,
  wherever they are, and its session ID. It carries no level.
- Cell and worldspace IDs need matching load orders.
- Non-goals: server authority over movement, anti-cheat, and UDP.
