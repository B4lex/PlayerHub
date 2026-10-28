# Network

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
| `network/SessionStore` | The session tokens in `PlayerHub.session`, one per server |
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
  `[Connection] ServerEndpoint` with `CustomServer` on,
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
- IXWebSocket is built as the `ixwebsocket-patched` package in `xmake.lua`:
  the xmake-repo recipe with `patches/ixwebsocket/happy-eyeballs.patch`,
  which makes `SocketConnect::connect` race the resolved addresses as in
  RFC 8305. The addresses alternate families, starting with the resolver's
  first; a new attempt starts every 250 ms, or at once when one fails, while
  the earlier ones stay open; the first to connect wins and the rest close.
  Stock IXWebSocket tried the addresses one after another under the
  handshake's single 10 s deadline, so a player whose system has IPv6
  configured but whose network drops it spent the whole deadline on the AAAA
  address and never reached IPv4. The deadline still covers resolving, every
  attempt, TLS, and the upgrade.
- The nanopb runtime is built from the `lib/nanopb` submodule at tag `0.4.9`,
  the generator version `proto/generate.sh` pins: the xmake `nanopb` package
  depends on `protoc`, whose install test fails under wine
  (`STATUS_DLL_NOT_FOUND`).
- The first client message must be
  `Hello{protocol_version, view_radius, session_token, display_name, vanished}`
  within `limits.hello_timeout_seconds` (5). A version other than
  `Protocol.PROTOCOL_VERSION` (14) is closed with status 4001 and reason
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
- Boundary limits: 20 KiB per message (the largest valid `ClientMessage`, an
  `Equipment`, is about 19 KB); `limits.messages_per_second` (120, burst 240) per connection,
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
| 4009 `too many players from address` | `limits.max_players_per_address` sessions are connected from the hello's address | refusal |
| 4010 `banned` | The session or the address is banned ([Kicks and bans](#kicks-and-bans)) | refusal |
| 4011 `kicked` | An admin kicked the player | refusal |
| 1008 | First message is not a hello, or `invalid session token` | reconnects |
| 1011 `session store unavailable` | The session store failed | reconnects |

An upgrade refused before the WebSocket exists answers HTTP 403 for a browser
([Transport](#transport)), 429 while the address has too many connections
waiting for their hello, or 503 while too many connections wait in all
([Admission](#admission)); the client treats each as a failed attempt and
retries. What a refusal means for the client is in [Connection](#connection).

## Client messages

| Message | Handling |
| --- | --- |
| `Hello` | Handshake, above. `view_radius` is the client's `[Advanced] ViewRadius` in game units, 0 for the server's ([Interest and delivery](interest-and-delivery.md#subscriptions)). `session_token` and `display_name` resume or issue the [session](#sessions). `vanished` lists the sessions the player [vanished](#vanish) on this server |
| `Enter`, `Exit` | World presence. `Exit` when not in the world does nothing |
| `Context` | A checkpoint's space and graph epoch ([below](#checkpoint-and-context)) |
| `Checkpoint` | One quantized sample |
| `Action` | One attack ([Actions](#actions)) |
| `Appearance`, `Equipment` | Latest-wins presentation ([below](#appearance-and-equipment)) |
| `ChatSend` | `{request_id, text, channel, recipient}`, answered with a `ChatResult` ([Chat](#chat)) |
| `Ping` | `{request_id}`, answered with `Pong{request_id}` by the connection at once; the latency probe |
| `Vanish` | `{session_id, vanished}`: stop or resume receiving a session's player ([Vanish](#vanish)) |
| `Inspect` | `{slot}`: ask for a player's level and race, answered with an `Inspection` ([Inspection](#inspection)) |
| `TradeRequest`, `TradeAnswer`, `TradeOffer`, `TradeReport`, `TradePluginQuery`, `TradePluginReply`, `TradeAccept`, `TradeCancel` | A trade between two players ([Trade](#trade)) |

A rejected message is dropped and logged on the server (at most one summary
per connection every 5 s); only `ChatSend` gets a `ChatResult` (`Ping` gets
its `Pong` from the connection), an accepted `Inspect` gets an `Inspection`,
and a refused `TradeRequest` a `TradeClose`. Graph
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
| `x`, `y`, `z` | `sint32`, quarter game units | any |
| `heading` | `uint32`, 1/4096 turn | `0..4095` |
| `pitch` | `sint32`, radians × 1024 | `|pitch| <= 1609` (π/2) |
| `speed` | `uint32`, units per second: the source graph's `Speed` while moving on foot, otherwise measured over the ground (3D while swimming) ([Locomotion](locomotion.md#source-capture)) | `<= 100000` |
| `direction` | `uint32`, graph `Direction` × 256, wrapping | `0..255` |
| `flags` | bits 0–2 movement state `0..6` (`idle`, `walking`, `running`, `sprinting`, `sneakingIdle`, `sneakingWalking`, `sneakingRunning`); bits 3–15 `grounded` (the carrier's controller is on the ground, [Locomotion](locomotion.md#ground-offset)), `weaponDrawn`, `jumping`, `blocking`, `bowDrawn`, `castingLeft`, `castingRight`, `swimming`, `mounted`, `dead`, `levitating`, `mistForm`, `bats` (`CheckpointFlag`, mirrored by `Network::CheckpointFlags` with `static_assert`s in `WireCodec.cpp`) | unknown bits rejected; the last three only in a `VAMPIRE_LORD` context (`validate.VampireLordFlags`, `incompatible animation checkpoint`) |
| `time` | `uint32`, the sender's `steady_clock` in milliseconds when sampled, modulo 16384 | `0..16383` |

| `Context` field | Validation |
| --- | --- |
| `interior_cell`, `worldspace` | [form references](#form-references) of the interior cell the player is in, or of the worldspace outside; never both. `interior_cell` is unset in a worldspace, so crossing exterior cells changes no field. Both are unset in a space without a form reference |
| `graph_epoch` | `>= 1`, never decreasing while in the world |
| `furniture` | occupied placed reference as a [form reference](#form-references), unset for none |
| `shapeshift` | `NONE`, `WEREWOLF`, `VAMPIRE_LORD`, or `WEREBEAR`: the vanilla beast form the player is in ([Appearance and equipment](appearance-and-equipment.md#shapeshifts)) |

`protocol/Quantization.h` and `server/internal/wire/quantize.go` hold the same
rules: positions round to quarter units (`kPositionScale`,
`wire.PositionScale`, 4) and speed to whole units, heading is normalized to
[0, 2π) first, pitch is clamped, and direction wraps (1.0 is 0). Whole-unit
positions changed the length of a 50 ms walking segment by about 12% (about 4%
running), and the receiver follows the interpolated path exactly, so it showed
as a change in speed at every checkpoint; quarter units cut that to a quarter.
A zigzag varint coordinate below 8192 in magnitude costs one byte more, others
the same (31.6 against 30.3 bytes per relayed entry with 100 bots within
±6000 units), and eighth units would add a byte above 131072. The server reads
positions in world units only through `wire.Position` and `wire.Units` (interest,
Nearby chat, trade range, the admin state, the bots). `time` is wall
time, not accumulated frame time, so a paused or hitching source does not
stretch it; receivers play checkpoints back at its spacing
([Sync pipeline](sync-pipeline.md#checkpoint-playout)). Sixteen seconds of range
keep it within two bytes; receivers recover the wraps from arrival times. The
server does not interpret it, and idle suppression ignores it with the
sequence ([Interest and delivery](interest-and-delivery.md#rate-classes-and-idle-suppression)).
The flag bits are ordered by how often they are set: a varint below 128 is one
byte, so the movement state with `grounded` and `weaponDrawn` costs one byte
on foot, where `grounded` at bit 15 cost three on almost every entry.
Virtual players and `cmd/loadgen` bots stamp their own
([Recording and replay](recording-and-replay.md#virtual-player)). The server
accepts a `Context` or `Checkpoint` only in the world, and a checkpoint only
after a context since the last `Enter` (`no context`). A context identical to
the stored one is dropped. Exterior cell IDs stay off the wire: nothing compares
them, and each exterior cell crossing would resend the context, force the next
checkpoint, and relay a `PeerContext` to every subscriber. The meaning of each field is in its owning system:
movement in [Locomotion](locomotion.md), stance and hands in
[Combat presentation](combat-presentation.md), furniture, mounts, and death in
[Native interactions](native-interactions.md), swimming in
[Remote actor](remote-actor.md#swimming).

## Actions

`Action` carries one attack or one beast action as a separate ordered stream,
since a checkpoint field would collapse repeated actions.

| Field | Validation |
| --- | --- |
| `action_sequence` | `>= 1`, exactly the previous plus one per player, shared by attacks and beast actions; the first one a server sees sets the boundary |
| `graph_epoch` | equal to the player's context epoch |
| `checkpoint_sequence` | `>= 1`, a published checkpoint no newer than the latest |
| `kind` | a oneof, required: `attack` or `beast` |
| `attack.action` | `NORMAL`, `LEFT`, `DUAL`, or `BASH` |
| `attack.direction` | `NONE` for an attack that is not a power attack, else `STANDING`, `FORWARD`, `BACKWARD`, `LEFT`, `RIGHT` |
| `beast` | a `BeastAction`: the `WEREWOLF_` values `1..19` (also the werebear's) or the `VAMPIRE_LORD_` values `32..51`; any other value is rejected |

For an attack, the server also requires the latest checkpoint to have the
weapon drawn and the context to have no shapeshift. A beast action needs a
context whose form plays that graph (`validate.SameBeastGraph`: a werebear
plays the werewolf's), and not the weapon drawn, since a beast howls and feeds
sheathed. Either needs a context without furniture, and the server allows
`limits.actions_per_second` (10, burst 10) per player. A duplicate, stale, or
gapped sequence is rejected rather than played out of order. Graph event names
stay out of the wire: the plugin maps each value to its event. The
vocabularies' meaning is in
[Combat presentation](combat-presentation.md#action-stream) and
[Combat presentation](combat-presentation.md#beast-actions).

## Appearance and equipment

Both are reconstructable state that changes rarely, so each has its own
latest-wins message with a source-local `revision` (`>= 1`, strictly rising
per connection). Capture, resolution, and fallbacks are in
[Appearance and equipment](appearance-and-equipment.md).

### Form references

Items, races, head parts, furniture, cells, and worldspaces are `FormRef{plugin, form_id}`: the
file that defines the form and its file-local ID (the low 24 bits, or 12 for a
light plugin). A receiver resolves by plugin name, so load orders need not
match. Plugin names are 1..260 bytes and `form_id` is `0..0xFFFFFF`. Dynamic
(`FF`) forms cannot be referenced and are omitted.

`FormLookup::GetDefiningFile` takes the plugin from the FormID's compile index
(or light index under `FE`), the same index the receiver's `LookupFormID`
rebuilds. `TESForm::GetFile(0)` is not used: a placed reference that a later
plugin overrides can list only the overriding file (Skyrim.esm's Arcane
Enchanter `000DDFB3` lists `Update.esm`), which would name a missing form on
every receiver.

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
| `face_parts` | exactly 4 `sint32`, each `-1..255` (`-1` for none); the plugin clamps to this range when capturing and parsing |
| `tints` | at most 64; `index` `0..65535`, `type` `0..14`, a color, `alpha` `0..1` |
| `default_race` | `{race, female}` with a base-game race, unset for none; receivers ignore `female`, which senders set to the appearance's `female` for 1.1.0 clients that still read it |

Hair color is RGB rather than a form, because character editors commonly
create dynamic color forms.

### `Equipment`

| Field | Validation |
| --- | --- |
| `armor` | at most 32, unique references; `slot_mask` non-zero (bit 0 is slot 30) |
| `right_hand`, `left_hand` | a hand, unset for empty |
| hand `kind` | `WEAPON`, `TORCH` (left hand only), or `SPELL` (no weapon type; the spell or scroll as `form`, unset when unreferenceable) |
| hand `casting_art`, `casting_light` | only for `SPELL`, each a form reference or unset: the spell's hand look, used when the receiver lacks the spell ([Combat presentation](combat-presentation.md#spell-look)) |
| hand `weapon_type` | `WEAPON_TYPE` `1..9`, only for a weapon; two-handed types (5, 6, 7, 9) only in the right hand, which then requires an empty left hand |
| `ammo` | form reference, unset for none |
| `default_outfit` | unset is empty |
| `beast_right_hand`, `beast_left_hand` | a `SPELL` hand, unset for empty: a vampire lord's spells, while the fields above keep the character's items ([Appearance and equipment](appearance-and-equipment.md#shapeshifts)) |
| `beast_armor` | at most 8, as `armor`: the vampire lord's worn armor |
| outfit `armor` | at most 8, non-zero slot masks |
| outfit `hands` | at most 2, a weapon with a weapon type or a torch, no casting art or light, no two with the same kind and weapon type |
| outfit references and `default_race.race` | from a base-game plugin, compared without case |

`slot_mask` and `weapon_type` are hints that select the default outfit piece;
the receiver's locally resolved form decides how an item is equipped. The
base-game plugins are the five Special Edition masters and
`ccFSVSSE001-Backpacks.esl`, without other Creation Club plugins;
`Multiplayer::IsBaseGamePlugin` and the server's `baseGamePlugins` hold the
same list.

## Server messages

| Message | Fields |
| --- | --- |
| `Welcome` | `slot`, `session_id`, `display_name`, and `session_token` only when the session was issued for this connection ([Sessions](#sessions)) |
| `ChatResult` | `request_id` and `rejection`: `NONE` accepts the `ChatSend`, else `INVALID`, `RATE_LIMITED`, `NOT_IN_WORLD`, or `UNKNOWN_RECIPIENT` ([Chat](#chat)) |
| `Pong` | `request_id` of the `Ping` |
| `PeerStates` | `repeated Checkpoint` with `player` set; the concatenation of entries encoded once per sender |
| `PeerEnter` | `slot`, `appearance` and `equipment` when newer than what the recipient holds, `context`, `state` (the stored checkpoint) |
| `PeerContext` | `slot`, `context` |
| `PeerLeave` | `slot`, `reason`: `OUT_OF_RANGE`, `EXCLUDED`, `EXITED`, `DISCONNECTED`, `VIEWER_UNPLACED` |
| `PeerAction`, `PeerAppearance`, `PeerEquipment` | `slot` and the message |
| `ChatMessage` | `channel`, `sender`, `recipient` (whisper only), `text`; each a `PlayerIdentity{slot, name, session_id}` |
| `Roster` | `joined`, a list of `PlayerIdentity`, and `left`, a slot or 0 ([Roster](#roster)) |
| `NearbyPlayers` | `joined` and `left`, packed slots entering and leaving the recipient's Nearby chat reach ([Nearby players](#nearby-players)) |
| `Inspection` | `slot`, `level`, `race`, `default_race` of the player an `Inspect` named ([Inspection](#inspection)) |
| `TradeInvite`, `TradeOpen`, `TradeOffer`, `TradeReport`, `TradePluginQuery`, `TradePluginReply`, `TradeAccept`, `TradeCommit`, `TradeClose` | A trade's progress ([Trade](#trade)) |

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
accounts, passwords, recovering a lost session, roles, and protecting the
admin API. Bans are in [Kicks and bans](#kicks-and-bans).

| Part | |
| --- | --- |
| `session_id` | `uint64` from 1, the row ID of an `AUTOINCREMENT` column, so it is never reused, also after the session is gone. Sent to its own client in `Welcome`, and to every client on roster entries and chat speakers, which is how a client mutes or vanishes a player whatever slot or name it connects with. The hub keeps it on the player (`world.Player.Session`) |
| Token | 32 bytes from `crypto/rand`. The store keeps only its SHA-256, so a copy of the database resumes nobody. It is returned once, in the `Welcome` of the connection it was issued for |
| Display name | 3..15 (`validate.MinimumDisplayNameBytes`, `validate.MaximumDisplayNameBytes`) ASCII letters, digits, and underscores (`validate.DisplayName`; the client's `Multiplayer::IsValidDisplayName` in `src/protocol/Session.h` is the same rule). Without spaces, a name typed before a whisper's message is told from the message. `Hello.display_name` and `Welcome.display_name` are nanopb buffers of 16 bytes. Unique among sessions by `validate.DisplayNameKey`, its lower case; a session may respell its own |
| Last use | Set by every hello, when a connection closes, and hourly for connected sessions (`session.Server.Maintain`) |

- `auth.Store` keeps sessions in SQLite (`modernc.org/sqlite`, pure Go) at
  `sessions.database` (`sessions.db`; empty keeps them in memory, for
  tests), with a write-ahead log and `user_version` 3. A version 1
  database, whose columns are named `nickname` and `nickname_key`, has them
  renamed when opened and keeps its sessions; versions 1 and 2 get the
  `bans` table. Its one connection serializes transactions, which the
  display name checks rely on. Store calls run on a connection's goroutine,
  never on the hub's.
- A session lives `sessions.lifetime_days` (30) after its last use, so one that
  keeps connecting never expires and none expires while connected. A banned
  session never expires ([Kicks and bans](#kicks-and-bans)). Expired
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
  `validate.DisplayName`, else 422; a held display name answers 409, and a
  banned session 403. It
  answers `{sessionId, displayName, token}`, 201 with a new token or 200 with
  the resumed one. A session issued there that never connects is not deleted
  by `Store.Abandon`; it expires.
- The admin API lists, renames, and deletes sessions
  ([Recording and replay](recording-and-replay.md#admin-api)).
  `session.Server.RenameSession` refuses a connected session
  (`session.ErrConnected`), whose name is fixed for the connection, and
  otherwise `Store.Rename` applies the same uniqueness rule as a hello, after
  deleting expired sessions; an expired or unknown session is
  `auth.ErrNoSession`. The player's next hello renames the session to the
  name it carries, as for any resumed session; an admin rename does not hold
  against it (the user's choice). `session.Server.DeleteSession` deletes the
  session and its bans in one transaction (`Store.Delete`) and closes its
  connection with 4011 `kicked`, so the game stays disconnected until the
  player presses Connect; its token then matches nothing and issues a new
  session with a new `session_id`, which ends friends, mutes, and vanishes
  other clients keep for the old one. The server logs `session renamed` and
  `session deleted` with the session.
- A client is named by its session everywhere: `Appearance.name` on the stored
  and relayed copy, `PlayerIdentity.name` in chat, the roster, and trade invitations, the admin state, and a recording's
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
- `limits.max_pending_hellos_per_address` (8; 0 is no cap) caps the same per
  address, counted like the session allowance below (IPv6 per /64), so one
  host holding sockets that never send a hello cannot fill the shared cap and
  refuse everyone else, including reconnecting players. It is checked first;
  over it, the upgrade answers HTTP 429. Eight leaves room for players behind
  one NAT joining together, since a hello normally ends within a round trip.
  Spreading the flood over many addresses is left to the proxy in front,
  which can rate-limit `/ws` per address before a connection reaches the
  server.
- `limits.max_players` (100; 0 is no cap) caps connected sessions. A hello
  without a token on a full server is closed with 4007 `server full` before
  the store is touched. Otherwise the session is admitted after it
  authenticates, unless the server is full and the session is not connected
  already: a session replacing its own connection is always admitted, so a
  reconnect never waits for its dead connection's timeout. A new session
  issued in the race between that check and admission is deleted again. The
  cap also bounds the roster every client receives.
- `limits.max_players_per_address` (3; 0 is no cap) caps the sessions
  connected from one address, counted like the session allowance below (IPv6
  per /64), so one host cannot hold more than a few of the `max_players`
  places with the sessions its allowance issued. It is checked with
  `max_players` and the same way, after it, and closes with 4009
  `too many players from address`: a hello without a token is refused before
  the store is touched, so a refused hello never spends the allowance, and a
  session already connected from that address is always admitted. A session
  connected from another address is not; it would add a player here.
  Three admits a few players sharing a home connection; a larger group on one
  address is refused, and players behind carrier-grade NAT share the cap with
  strangers. It does not stop a flood spread over many addresses.
  `cmd/loadgen` connects every bot from one address and needs the cap off.
- `limits.new_sessions_per_address_per_month` (10; 0 is no limit) is a token
  bucket per address with that burst, refilled at that many per 30 days. The
  buckets live in memory, so a server restart refills every allowance.
  Resuming a session never spends it, and neither does a session issued
  through the admin API ([Sessions](#sessions)). An IPv6 address counts with
  its /64, which one subscriber usually holds whole. `Server.Maintain` prunes
  full buckets hourly.
- The address is the TCP peer's, unless the peer is in `trusted_proxies`
  (addresses or CIDR prefixes, empty by default). Then it is the rightmost
  `X-Forwarded-For` hop not in `trusted_proxies`: each proxy appends the
  address it received from, so earlier hops may be forged by the client. A
  hop that does not parse stops the walk at the last trusted address. Behind
  a proxy without `trusted_proxies`, every player shares the proxy's
  allowance.
- Upgrades refused for either hello cap are logged as a throttled summary
  (`refused connections`, with the cap as its reason), since a flood would
  repeat them.

### Kicks and bans

Goal: an admin can remove a connected player from the server, and keep one
out. Both act from the admin API
([Recording and replay](recording-and-replay.md#admin-api)); replays and
in-server bots have no connection and cannot be kicked or banned. Non-goals:
timed bans and a reason the player sees (the user's choices: a ban lasts
until lifted, and no admin text reaches clients, which show a fixed notice
for a kick and a fixed connection controls sentence for a ban,
[UI overlay](ui-overlay.md#notices)), banning a player who is not
connected, and stopping a player who changes both session and address.

- `session.Server.Kick` closes the player's connection with 4011 `kicked`.
  The plugin takes it as a refusal, so the game stays disconnected until the
  player presses Connect ([Connection](#connection)); the session is kept
  and may connect again at once.
- `session.Server.Ban` stores a ban and closes the connection with 4010
  `banned`. A ban always holds the session; the admin chooses per ban whether
  it also holds the address the player connected from, keyed like the
  admission caps (an IPv4 address or an IPv6 /64). An address ban also
  closes every other connection from that address with 4010.
- An address ban is refused (`session.ErrAddressNotBannable`) when the
  address is loopback, unspecified, or inside `trusted_proxies`: that is a
  proxy's address, which every player behind it shares when the proxy is not
  trusted or forwards no address.
- A hello from a banned address is closed with 4010 as soon as it is read,
  before the admission checks, so a banned player learns of the ban on a full
  server too. A hello whose token resumes a banned session is closed with
  4010 by `Store.Authenticate` (`auth.ErrBanned`), before any rename. A
  session-only ban leaves the address free to issue a new session within its
  allowance.
- `auth.Store` keeps bans in the `bans` table: `session_id`, the
  `display_name` it was banned under, `address` as a prefix or NULL, and
  `created_at`. A banned session never expires and `Store.Abandon` keeps it,
  so its token keeps matching the ban and its display name stays held.
  `Store.Lift` deletes a ban and restarts its session's lifetime from then.
  Deleting a session deletes its bans with it ([Sessions](#sessions)), so it
  also lifts an address ban the session held.
- The server logs `player kicked` and `player banned` with the session and
  display name, and the ban's ID and address.

### Chat

`ChatChannel` is `GLOBAL` (0), `NEARBY`, or `WHISPER`.

- The server trims surrounding spaces and rejects an unknown channel, empty
  text, control characters (line breaks included), more than 512 characters
  counted as code points (`validate.MaximumChatTextRune`,
  `Multiplayer::kMaximumChatTextCharacters`) or 2048 UTF-8 bytes, a
  `WHISPER` without a `recipient`, and a `recipient` on another channel
  (`invalid chat message`, rejection `INVALID`); protobuf already guarantees
  valid UTF-8.
- `GLOBAL` goes to every connected client.
- `NEARBY` needs a placed sender (`not in world`, `NOT_IN_WORLD`, otherwise) and goes to every
  placed client in the same space: in a worldspace within `chat.nearby_radius`
  (8192, horizontal like interest), and in an interior to the whole cell. It
  ignores exclusions, hiding, and the visible cap, which govern presentation
  only. Recipients are found by a scan of connected clients, which the rate
  limit keeps cheap; the interest grid does not bound an interior.
- `WHISPER` names its recipient by slot, which the client takes from the
  [roster](#roster). A slot is one connection, so a whisper never reaches
  whoever holds the recipient's display name after it left, and names with
  spaces need no parsing on the server. A slot that is not a connected client
  is `unknown recipient` (`UNKNOWN_RECIPIENT`), which includes replays and bots, since they receive
  nothing. The whisper goes to the sender and the recipient (once when they
  are the same).
- 5 accepted messages per player in any 5 s over all channels
  (`rate limited`, `RATE_LIMITED`); rejected messages do not count, and a disconnect forgets
  the history.
- Every `ChatSend` is answered with a `ChatResult` naming the rejection, or
  `NONE`; the hub maps its error (`hub.chatRejection`), and the client
  chooses the sentence it shows from the value, so server wording never
  reaches the player.
- Every channel includes the sender, so everyone sees one order and the
  sender's own line confirms delivery. A speaker is the player's
  `PlayerIdentity`: slot, display name, and session ID.
- The client rejects a `ChatMessage` without a sender, with an unknown channel,
  or with a recipient present exactly when the channel is not `WHISPER`, and
  validates speakers like roster entries, and text like `ChatSend`
  (`Multiplayer::IsValidChatText`). `ChatSend.text` and `ChatMessage.text`
  are nanopb buffers of 2049 bytes, room for 512 four-byte characters. A
  `ChatResult` with an unknown rejection is rejected.
- Chat does not require being in the world except for `NEARBY`, keeps no
  history, and is not recorded. The client side is in
  [UI overlay](ui-overlay.md#chat). Accepted `GLOBAL` lines are also pushed
  to listening admin panels ([Admin API](recording-and-replay.md#admin-api)).

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
  name, or an invalid name (`ReadPlayerIdentity`, the rule for every
  `PlayerIdentity`), or with more than `kMaximumRosterEntries` (4096)
  entries.

### Nearby players

Every client knows which listed players its Nearby lines reach, so the
[player list](ui-overlay.md#player-list) shows them in the Nearby color. The
client cannot work it out: it receives at most `max_visible` peers within its
view radius, while Nearby reaches a whole interior.

- The set follows [Nearby chat](#chat) (`Hub.hearsNearby`): the listed
  players placed in the same space, in a worldspace within
  `chat.nearby_radius`, and in an interior all of it. The rule is mutual, and
  like Nearby chat it ignores exclusions, hiding, vanish, and the cap.
- `Hub.updateNearbyPlayers` runs after each interest pass and scans the
  listed clients due, each a second after its previous scan
  (`nearbyPlayersInterval`), so a crowd's scans spread over the passes and a
  change shows in the list up to about 1.2 s late, plus its half-second copy.
  With 500 `cmd/loadgen` bots in each other's reach, scanning every viewer on
  every interest pass instead cost 23% of the relayed entries and 20 ms of
  p99 latency. A
  scan finds candidates in `nearbyGrid`, a second `interest.Grid` whose cells
  are `chat.nearby_radius` wide, where an interior's players all share one
  cell (`nearbyView`). It checks each candidate against the slots it sent
  before and walks them again only when one may have left.
- A change is sent as one `NearbyPlayers` with the joined and left slots,
  through `Outbox.AppendGlobal` and so in order with the roster. An unplaced
  client's set empties at its next scan. A disconnect is not sent: the
  roster's `left` removes the slot on the client too, and the hub forgets it
  in every set.
- The client rejects a `NearbyPlayers` with a zero slot or with more than
  `kMaximumRosterEntries` (4096) slots in either list, and accepts both the
  packed and unpacked encoding.

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
  nothing. Neither gets a reply, and an unknown session is accepted: the
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

### Trade

The server's half of [Trade](trade.md): it pairs two players, relays their
offers in order, checks range, and tells both when to apply the exchange. It
never sees an inventory and does not check what an offer holds beyond its
form.

Client messages:

| Message | Fields | Validation and handling |
| --- | --- | --- |
| `TradeRequest` | `slot` | `slot` non-zero. Only a client with a session asks; replays and bots are ignored. Refused with `TradeClose{0, reason}`: `UNAVAILABLE` for an unlisted slot, oneself, a player the requester vanished, or over the rate limit (0.5 per s, burst 3, `tradeRequestsPerSecond`, `tradeRequestBurst`); `BUSY` when either player is in a trade; `OUT_OF_RANGE` unless both are placed in the same space within `trade.request_radius`, horizontally (`interest.Distance`); `TRANSFORMED` when either player's context has a shapeshift. Otherwise both get `TradeInvite`, except that a target who vanished the requester is not told |
| `TradeAnswer` | `trade_id`, `accept` | `trade_id` non-zero; only the invited player, while invited (`unknown trade`, `stale trade message`). Declining closes with `DECLINED`; accepting opens the trade, or closes it with `OUT_OF_RANGE` when the players are over `trade.keep_radius` apart |
| `TradeOffer` | `trade_id`, `revision`, `items` | `trade_id` and `revision` non-zero, at most 15 items, each a valid `TradeItem`; only while open, with a revision above the player's last. 10 per s, burst 20 (`tradeOffersPerSecond`, `tradeOfferBurst`). Clears both acceptances and is relayed to the other player as it is |
| `TradeReport` | `trade_id`, `revision`, `skipped` | `trade_id` and `revision` non-zero, at most 15 `TradeSkip`s with unique `index` `0..14`, a known `reason`, and `plugin` of at most 260 bytes without control characters. Only while open, for the other player's current revision, once per revision, with indices below that offer's item count. Relayed to the other player |
| `TradePluginQuery` | `trade_id`, `plugins` | `trade_id` non-zero, 1 to 256 plugin names (`validate.MaximumTradePlugins`), each 1 to 260 UTF-8 bytes without control characters; duplicates are allowed. Only while open, at most 8 unanswered per player (`tradePendingPluginQueries`, `rate limited`), and sharing `TradeOffer`'s rate. The hub keeps each query's name count and relays it to the other player as it is ([plugin check](trade.md#plugin-check)) |
| `TradePluginReply` | `trade_id`, `missing` | `trade_id` non-zero, at most 256 unique indices below 256. Only while open, answering the other player's oldest unanswered query, with every index below its name count (`stale trade message` otherwise, also without a query). Relayed to the other player |
| `TradeAccept` | `trade_id`, `own_revision`, `peer_revision` | `trade_id` non-zero; only while open, with both revisions current and this player's report on the other's revision received. Sent to the other player in its terms (`own_revision` its own, `peer_revision` this player's), or, when that player's acceptance stands, both get `TradeCommit` and the trade ends |
| `TradeCancel` | `trade_id`, `reason` | `trade_id` non-zero, `reason` one of `CANCELLED`, `COMBAT`, `MENU`, `FAILED`, `TRANSFORMED`; either player, invited or open. Both get `TradeClose` with the reason |

`TradeItem`:

| Field | Validation |
| --- | --- |
| `item` | required form reference |
| `count` | `1..2^31-1`; at most 1000 (`validate.MaximumTradeInstances`) for an item with an enchantment, effects, health, a name, or a soul, since a receiver creates each such unit alone |
| `enchantment` | form reference, unset for none; a plugin enchantment |
| `effects` | at most 4, exclusive with `enchantment`: a player enchantment's `{effect, magnitude, area, duration, cost}`, `effect` a required form reference, `magnitude` finite `0..1e6`, `area` and `duration` `0..1e6`, `cost` finite `0..1e7` |
| `maximum_charge`, `charge` | `0..65535`, `charge` finite; both zero without an enchantment or effects |
| `health` | finite `0..100`, `0` untempered |
| `name` | a name the player gave the item, at most 128 UTF-8 bytes, no control characters |
| `soul` | `0..5`, `SOUL_LEVEL` |

Server messages:

| Message | Fields |
| --- | --- |
| `TradeInvite` | `trade_id`, `sender` and `recipient` as `PlayerIdentity`s; sent to both players |
| `TradeOpen` | `trade_id`; to both |
| `TradeOffer`, `TradeReport`, `TradePluginQuery`, `TradePluginReply` | The other player's message as it sent it, so a `TradePluginReply` answers the recipient's oldest unanswered query |
| `TradeAccept` | The other player's acceptance, rewritten by the hub so that, as in `TradeCommit`, `own_revision` is the recipient's and `peer_revision` the other player's |
| `TradeCommit` | `trade_id`, `own_revision` (the recipient's), `peer_revision` |
| `TradeClose` | `trade_id`, 0 for a refused request; `reason`: `DECLINED`, `EXPIRED`, `CANCELLED`, `BUSY`, `OUT_OF_RANGE`, `COMBAT`, `MENU`, `DISCONNECTED`, `FAILED`, `UNAVAILABLE`, `TRANSFORMED` |

- Trade IDs are a `uint32` counter per server run, never 0. A trade is held
  by the hub (`trade`, `member.trade`), and each player's pointer is set
  only while it knows of it, so an invitation hidden by a vanish leaves the
  invited player free.
- Each checkpoint, a context that moves to another space, and `Exit` of a
  player in a trade check the distance to the other player; over
  `trade.keep_radius`, in another space, or unplaced closes it with
  `OUT_OF_RANGE`. A disconnect closes it with `DISCONNECTED`. A context
  with a shapeshift closes an invited or open trade with `TRANSFORMED`
  (`checkTradeForm`), since a beast form has no inventory.
- `Hub.UpdateInterest` expires invitations older than
  `trade.invite_seconds` with `EXPIRED`.
- A commit is logged (`trade completed`, both slots and item counts).
  Trades are not recorded or replayed.
- `[trade]` in the server's configuration: `request_radius` (400),
  `keep_radius` (1000, at least `request_radius`), `invite_seconds` (30).

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
- The transport keeps the [nearby players](#nearby-players) sorted, with the
  roster under its lock: `NearbyPlayers` adds and removes slots, a roster's
  `left` removes one, and a welcome and a disconnect empty them.
  `Network::CopyNearbyPlayers` appends them; chat orders its name list and
  colors its player list by them.
- `Network::DescribeServer` gives `the official server` for the official
  endpoint in either form and `a custom server` for any other. Every log line
  that names a server goes through it, so a log never carries a server
  address and players can share it: a failed or abandoned connection,
  vanishes over the limit, saving a session, and muting or vanishing a
  player.
- Trade messages are decoded like the rest, a `TradeOffer` entry by entry
  and a `TradePluginQuery` name by name as for `PeerStates`, and validated
  as the server does, plus a `TradeClose` with an unknown reason, a
  `TradeInvite` without both speakers, and a report index of 15 or more are
  rejected. A query is encoded only within
  15 KiB by `EstimateTradePluginBytes`, under the server's message limit. `TradeInvite` is marked
  `outgoing` when its sender is the connection's own slot. The events are
  queued in arrival order and drained as
  `RemotePlayerChanges::tradeEvents`; over 256 waiting they are dropped and
  `tradeEventsLost` is set, so the trade ends rather than skip a message.
  The queue empties with a lost connection. `Network::PublishTrade*` send
  while connected; `PublishTradeRequest` reports whether it did.
- `Network::RequestInspection` sends `Inspect` while connected and reports
  whether it did. An `Inspection` without a slot, with a level above 65535,
  with an invalid race, or with a default race that is invalid or not from a
  base game plugin is rejected, as an `Appearance` with them would be. The
  newest per slot is queued with the remote changes, drained as
  `RemotePlayerChanges::inspections`, and dropped with them when the
  connection is lost.

### Session

- `SessionStore` keeps one token per server in `PlayerHub.session`, next to
  `PlayerHub.ini` (`Config::GetDirectory`): a line of 64 hexadecimal digits, a
  space, and the WebSocket URL the connection is opened to; lines starting with
  `;` are comments. It is a file of its own because players share their ini and
  modpacks, and a token is all it takes to connect as its owner; one per server
  lets a player switch servers and back. The file is read once, on first use,
  and written through `PlayerHub.session.tmp`. Tokens are never logged.
- When the socket opens, the transport thread sends the hello with the token
  kept for the attempt's URL, or none, and `[Connection] DisplayName`.
- A `Welcome` with a token saves it, also when the connection is already being
  closed, since the session exists on the server either way. A failed save is
  logged as an error: the token lasts until the game exits.
- The welcome's session ID and display name are only logged; the roster
  carries the local player's session ID too.
- The client sends no `Appearance.name`: the hello carries the display name,
  which holds for the connection ([Connection](#connection)).

### Player filters

- `PlayerFilterStore` keeps the local player's friends and the players it
  muted or vanished in `PlayerHub.players`, next to `PlayerHub.session`: a
  line per player and server, any of `f`, `m`, and `v` in that order, the
  session ID, the WebSocket URL, and the name when last seen
  ([UI overlay](ui-overlay.md#friends)), separated by tabs, which neither a display
  name nor a URL holds; lines starting with `;` are comments, and a line
  without a session, a server, a name, or a known flag is skipped with a
  warning. It is read once, on first use, and written through
  `PlayerHub.players.tmp` on every change; a failed write lasts until the
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
once `[Connection] DisplayName` is set
([Appearance and equipment](appearance-and-equipment.md#display-name)), and
while `Config::GetServerEndpoint` is not empty or a connection is welcomed.
The endpoint is empty only with `CustomServer` on and no `ServerEndpoint`
([Configuration](configuration.md#official-and-custom-server)).
`ConnectionMonitor` owns that decision; nothing else opens or closes the
client.

- At `kDataLoaded`, `Network::Initialize` only initializes the socket library;
  the main menu never connects.
- A game starts when `PresenceSync` first finds the player in the world, and
  ends when the main menu opens (`GameSession`). Loading screens keep the game
  and its connection. For the connection, a game starts only once the
  [vanilla intro](#vanilla-intro-hold) no longer holds it.
- At a game start the choice becomes `[Connection] ConnectAutomatically`
  (default `true`) if a display name and an endpoint are set, and disabled
  otherwise. The settings window's Connect, Cancel, and Disconnect change it with
  `Network::SetConnectionEnabled` until the game ends.
- Applying a display name never opens a connection; only the choice does. The
  monitor reads the display name when the configuration revision changes, and
  one that becomes empty disables the choice, so entering one again waits for
  Connect. The server requires one too, in the hello.
- Applying an endpoint or switching `CustomServer` never touches the
  connection: a welcomed one stays with its server, and each later attempt,
  after Connect or a loss, reads `Config::GetServerEndpoint` when it begins.
  Goal: changing the server setting does not drop players from the server
  they are playing on; Disconnect and Connect switch servers. An endpoint
  that is empty while no connection is welcomed disables the choice and
  closes the client, so entering one waits for Connect.
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
- `MakeProtocolRefusal` turns a 4001 into `kServerOutdated` when the
  version in its `protocol <server version>` reason is below
  `Wire::GetProtocolVersion`, and into `kClientOutdated` otherwise, also when
  the reason does not parse, since an outdated client is the usual cause:
  each release ships its server update. Both versions are logged.
- A close with status 4001 or 4004 through 4011 is a refusal:
  retrying at once cannot help, so the monitor disables the player's choice and closes the client,
  which leaves `kDisconnected`. `Network::GetConnectionRefusal` returns its
  `Refusal{reason}` for the settings window's [connection
  controls](ui-overlay.md#connection-controls) until the player presses
  Connect or a game starts or ends. Nothing retries by itself, also not an
  applied display name or endpoint: the player changes what the refusal names
  and presses Connect. 4006 is a refusal because retrying would close the
  connection that replaced this one, and two games holding one token would
  take the session from each other without end.

The monitor's timers accumulate the clamped frame delta, which is zero in the
main menu and while paused, so they hold while opening and closing still follow
the game.

### Vanilla intro hold

Goal: a player whose new game starts with the vanilla prisoner intro (the
cart ride, the execution, and Alduin's arrival) is not put on a server
during it, where they cannot move and other players would only watch the
scripted scene. Multiplayer starts once the player gets control back after
Alduin's knockdown. Non-goals: holding the connection in other scenes or
cutscenes; for a start a mod puts in place of the cart, including one that
keeps MQ101 running or lets the player watch Helgen's attack as a visitor;
and for a cart ride a player begins later in the play-through, as
Alternate Perspective allows, because the player has already been playing
online. With a skipped intro the game connects at once.

- While `VanillaIntro::IsPlaying` holds ([Sync pipeline](sync-pipeline.md#vanilla-intro)),
  no connection is wanted (`IsConnectionWanted`), the monitor closes any open
  one with the reason `the vanilla intro is playing`, and
  `Network::GetConnectionState` reports `kDisconnected` once the socket is
  closed. The hold is evaluated in the client only; the server knows nothing
  of it.
- The end of the hold is the game start for the connection, so
  `ConnectAutomatically` connects then, and a refusal is cleared then, as at
  any game start.
- Loading a save from the prisoner intro before the knockdown holds the
  connection again, and closes one that is open.
- During the hold the settings window cannot connect, and hovering its
  disabled button says why ([UI overlay](ui-overlay.md#connection-controls));
  the [display name hint](ui-overlay.md#display-name-hint) stays hidden. The
  status panel, chat, and the rest of the overlay behave as while
  disconnected.

### Loss and recovery

- A server restart, a missed server ping, or a network loss reaches the client
  as a WebSocket close or error. Each marks the client disconnected and forgets
  every remote player.
- A server that stops answering while the socket stays open is found by the
  latency probe: no `Pong` for 5 s of accumulated frame time makes the
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
send to `Pong`, so frame rate does not affect it. Each ping has a request ID;
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
   appearance and equipment.
2. A client that joins while a peer's game is paused sees that peer at once.
3. A client with another protocol version tells the player in the settings
   window whether the server runs a newer or an older version of the mod, and
   stays disconnected until Connect.
4. A paused game (menu open) stays connected for minutes.
5. Quantization shows no visible jitter in position or heading.
6. A client without a display name opens no connection, whatever
   `ConnectAutomatically` says, and cannot press Connect; applying one does not connect until Connect
   is pressed.
7. A first connection to a server writes its line to `PlayerHub.session`; later
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
11. A second game started with a copy of `PlayerHub.session` takes the session;
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
15. A new vanilla game with `ConnectAutomatically` and a display name opens
    no connection during the cart ride, the execution, or Alduin's attack,
    and connects by itself when the player can move after the knockdown. The
    display name hint does not show before then, and Connect is disabled.
16. A new game whose start quest is not MQ101 (an alternate start), or a save
    past the knockdown, connects at once; a save from the prisoner intro
    before the knockdown loaded while connected disconnects until the player
    can move.
17. With Alternate Perspective, its own start connects when its race menu
    closes, a save from it connects at once, and neither its Helgen visitor
    path nor its cart branch taken after the start disconnects a connected
    player.

## Known limitations

- Sessions authenticate players only. The admin API can delete recordings and
  spawn virtual players without one; it is served only on `admin_listen`,
  which must not face the Internet, and refuses only browser requests from
  other sites ([Recording and replay](recording-and-replay.md#admin-api)).
- The `Origin` rule stops browsers only. Any other program can open
  connections and send hellos from its own address.
- A session cannot be recovered: a player who loses `PlayerHub.session`, or
  whose plugin could not write it, gets a new session, and the old one keeps
  its display name until it expires.
- Display names can still be squatted from many addresses, slowly from one
  (`limits.new_sessions_per_address_per_month`), or by a program, which the
  `Origin` rule does not stop. Resumed hellos are not limited per address.
- A full server or an exhausted allowance leaves the player disconnected until
  Connect; nothing retries when a place frees.
- Display names compare by lower case only, so look-alikes such as `l` and
  `I` or `0` and `O` are different display names.
- Non-goal: display names in other scripts, with spaces, or with punctuation
  besides `_`. The narrow rule keeps names typeable in a whisper, renderable by
  every font, and comparable without Unicode case folding.
- The token is sent in the hello as it is, so an `http://` endpoint exposes it
  to the network; `https://` endpoints carry it under TLS.
- The official URL is kept out of the settings window and the log only: it
  is a plain string in the DLL, the key of its lines in `PlayerHub.session`
  and `PlayerHub.players`, and IXWebSocket's reason for a failed connection
  is logged as the library gives it, which may name the host.
- Admin exclusions and hiding are still keyed by slot
  ([Interest and delivery](interest-and-delivery.md#admin-controls)); only a
  player's own vanish follows the session.
- Chat has no history for late joiners.
- The roster tells every client the display name of every connected player,
  wherever they are, and its session ID. It carries no level.
- Form references, spaces included, match only between peers that load the
  defining plugin under the same file name: a merged or renamed plugin is
  another plugin.
- Non-goals: server authority over movement, anti-cheat, and UDP.
