# Network

## Scope

This system owns the wire protocol, the client transport and connection
management, and the server's connections, player [sessions](#sessions),
validation, [roles](#roles), and per-player state. It validates untrusted
fields at the boundary and queues plain values for the
[sync pipeline](sync-pipeline.md#receive-state). Which peers a client receives
and in what order is [Interest and delivery](interest-and-delivery.md).

Each client release ships with its server, so the protocol evolves without
compatibility shims; a version mismatch is refused at the handshake.

## Components

| Module | Responsibility |
| --- | --- |
| `proto/playerhub/v1/` | The schema (`state.proto`, `messages.proto`) and nanopb size options. `proto/generate.sh` regenerates `server/internal/wire/pb` and `src/network/proto` |
| `src/protocol/` | Plain wire values and shared validation: `PlayerState.h`, `MovementState.h`, `AppearanceState.h`, `PlayerInspection.h`, `EquipmentState.h`, `FormReference.h`, `ChatMessage.h`, `PeerSlot.h`, `Session.h`, `Role.h`, `VoiceFrame.h`, and the quantization rules in `Quantization.h`. No Skyrim headers |
| `network/WireCodec` | nanopb encoding of client messages and validated decoding of server messages |
| `network/SkeletonDelta` | Writes a skeleton pose as a [change](#pose-changes) from its stream's previous one, and resolves a received pose against it |
| `network/ServerClient` | IXWebSocket transport: sends, dispatches received messages, holds both directions' [pose references](#pose-changes), merges each peer's context into its checkpoints, and queues per-player changes for `DrainRemotePlayerChanges` |
| `network/ConnectionMonitor` | When to connect, reconnect schedule, latency probes, refusal, and the connection state |
| `network/SessionStore` | The session tokens in `PlayerHub.session`, one per server |
| `network/Refusal.h` | Why the server refused a connection, for the monitor and the UI |
| `network/TrafficMeter` | Bytes per second in each direction |
| `network/VoiceTransport` | The voice datagram socket and its link to the relay ([Voice](#voice)) |
| `server/internal/session` | WebSocket accept, hello and session authentication, reader and writer goroutines, liveness, per-connection limits |
| `server/internal/auth` | The session store: tokens, unique display names, expiry, bans, roles |
| `server/internal/admission` | Caps on players, pending hellos, and new sessions per address; the client address behind trusted proxies |
| `server/internal/validate` | Field validation of client messages |
| `server/internal/world` | A player's accepted state and the ordering rules |
| `server/internal/wire` | Hand-written encoding of checkpoint entries and batches, the skeleton pose codec with [changes](#pose-changes), quantization for tools |
| `server/internal/hub` | Applies accepted messages on one goroutine ([Interest and delivery](interest-and-delivery.md)) |
| `server/internal/voice` | The voice relay: datagram keys and addresses, replay windows, forwarding ([Voice](#voice)) |

## Transport

- One binary WebSocket message carries one `ClientMessage` or `ServerMessage`,
  each a protobuf `oneof`. The server listens at `/ws` on `listen`; the admin
  panel and API are served only on `admin_listen`
  ([Recording and replay](recording-and-replay.md#admin-api)). Voice also
  travels as encrypted UDP datagrams on `voice.listen`, outside the WebSocket
  ([Voice](#voice)).
- The server refuses with 403 an upgrade whose `Origin` scheme is not `ws` or
  `wss` (`session.playerOrigin`), so a web page cannot make its visitors'
  browsers join (a browser sends the page's origin). IXWebSocket sends
  `ws://host:port` or `wss://host:port` and Go clients none, so both join. The
  origin's host is not compared with `Host`, since behind a proxy the plugin
  names the public host.
- The server address (`Config::GetServerAddress`: the official server, or
  `[Connection] ServerAddress` and `ServerTls` with `CustomServer` on,
  [Configuration](configuration.md#official-and-custom-server)) is a host,
  a port, and whether to use TLS. `ServerClient`'s `ToWebSocketUrl` makes it
  `wss://host:port/ws` or `ws://host:port/ws`, without the port when it is
  the scheme's own. That URL keys the server's lines in `PlayerHub.session` and
  `PlayerHub.players`, so `example.org` and `example.org:14201` are one server.
  The server is always reached at `/ws`; TLS is terminated by a reverse proxy
  in front of it.
- The client's TLS is IXWebSocket 12 on mbedtls, pinned to 3.6 in `xmake.lua`,
  verifying against the Windows certificate store. mbedtls 4 returns
  `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` for every TLS 1.3 session
  ticket, which IXWebSocket treats as a failed read. The
  `package.librarydeps.strict_compatibility` policy rebuilds IXWebSocket when
  mbedtls changes, since it selects the mbedtls API at compile time.
- IXWebSocket is built as the `ixwebsocket-patched` package: the xmake-repo
  recipe with `patches/ixwebsocket/happy-eyeballs.patch`, which makes
  `SocketConnect::connect` race the resolved addresses as in RFC 8305
  (alternating families from the resolver's first, a new attempt every 250 ms
  or at once when one fails, the first to connect wins), so a network that
  drops configured IPv6 still reaches IPv4 within the handshake's 10 s
  deadline, which covers resolving, every attempt, TLS, and the upgrade.
- The nanopb runtime is built from the `lib/nanopb` submodule at tag `0.4.9`,
  the generator version `proto/generate.sh` pins, since the xmake `nanopb`
  package needs `protoc`, whose install fails under wine.
- The first client message must be
  `Hello{protocol_version, view_radius, session_token, display_name, vanished}`
  within `limits.hello_timeout_seconds` (5). A version other than
  `Protocol.PROTOCOL_VERSION` (17) is closed with status 4001 and reason
  `protocol <server version>`; anything else first is closed with 1008. The
  session fields are in [Sessions](#sessions).
- The server answers a hello with
  `Welcome{slot, session_id, display_name, session_token, voice}`, `voice`
  only while the server carries voice ([Voice](#voice)). Slots are `uint32`
  from a counter that only rises and are never reused within a server run, so a
  slot names one connection. Replays and in-server bots get slots too.
- Liveness runs on the transport, not the game: the server pings every
  `liveness.ping_interval_seconds` (5) and closes a connection whose pong does
  not arrive within `liveness.timeout_seconds` (10). IXWebSocket answers pings
  on its own thread, so a paused game stays connected. The client sends no
  WebSocket pings; its ping interval is -1, since IXWebSocket polls with the
  interval as timeout and 0 spins its thread.
- Boundary limits: 48 KiB per message (the largest valid `ClientMessage`, an
  `Appearance` with every tint and overlay path at full length, is about 44 KB); `limits.messages_per_second` (120, burst 240) per connection,
  over which the connection is closed with 4003, counting every message; a recipient whose unsent
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
| `Action` | One attack, beast action, idle event, or power ([Actions](#actions)) |
| `Appearance`, `Equipment`, `Effects` | Latest-wins presentation ([below](#appearance-and-equipment)) |
| `ChatSend` | `{request_id, text, channel, recipient}`, answered with a `ChatResult` ([Chat](#chat)) |
| `Ping` | `{request_id}`, answered with `Pong{request_id}` by the connection at once; the latency probe |
| `Vanish` | `{session_id, vanished}`: stop or resume receiving a session's player ([Vanish](#vanish)) |
| `Inspect` | `{slot}`: ask for a player's level and place, answered with an `Inspection` ([Inspection](#inspection)) |
| `TradeRequest`, `TradeAnswer`, `TradeOffer`, `TradeReport`, `TradePluginQuery`, `TradePluginReply`, `TradeAccept`, `TradeCancel` | A trade between two players ([Trade](#trade)) |

A rejected message is dropped and logged on the server (at most one summary
per connection every 5 s); only `ChatSend` gets a `ChatResult` (`Ping` gets
its `Pong` from the connection), an accepted `Inspect` gets an `Inspection`,
and a refused `TradeRequest` a `TradeClose`. Graph
event names cross the network only as [idle events](#actions).

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
| `look_pitch` | `sint32`, radians × 1024: the player's view pitch while the weapon is drawn, 0 otherwise ([Combat presentation](combat-presentation.md#aim)) | `|look_pitch| <= 1609` (π/2) |
| `speed` | `uint32`, units per second: the source graph's `Speed` while moving on foot, otherwise measured over the ground (3D while swimming) ([Locomotion](locomotion.md#source-capture)) | `<= 100000` |
| `direction` | `uint32`, graph `Direction` × 256, wrapping | `0..255` |
| `flags` | bits 0–2 movement state `0..6` (`idle`, `walking`, `running`, `sprinting`, `sneakingIdle`, `sneakingWalking`, `sneakingRunning`); bits 3–23 `grounded` (the carrier's controller is on the ground and the carrier is not ragdolled, [Locomotion](locomotion.md#ground-offset)), `weaponDrawn`, `jumping`, `blocking`, `bowDrawn`, `castingLeft`, `castingRight`, `swimming`, `mounted`, `dead`, `levitating`, `mistForm`, `bats`, `castReleasedLeft`, `castReleasedRight`, `dualCasting` ([Combat presentation](combat-presentation.md#spells)), `shouting`, `shoutReleased`, `shoutTwoWords`, `shoutThreeWords`, `shoutFailed` ([Combat presentation](combat-presentation.md#shouts-and-powers)); bit 24 `flyingMount` (the mount's race flies); bits 25–27 the flying mount's fly state `0..6` (`FlyState`: `none`, `takeOff`, `cruising`, `hovering`, `landing`, `perching`, `action`, Skyrim's `FLY_STATE`, [Native interactions](native-interactions.md#dragon-flight)); bits 28–29 its wing stroke while it cruises (`WingState`: `none`, `flapping`, `gliding`, `feathering`, [Native interactions](native-interactions.md#wing-strokes)) (`CheckpointFlag`, mirrored by `Network::CheckpointFlags` with `static_assert`s in `WireCodec.cpp`) | unknown bits rejected; `flyingMount` only with `mounted`, a nonzero fly state or wing stroke only with `flyingMount`, and the fly state at most `action` (`validate.mountFlags`, `IsValidQuantizedCheckpoint`); `levitating`, `mistForm`, and `bats` only in a `VAMPIRE_LORD` context (`validate.VampireLordFlags`, `incompatible animation checkpoint`); `castReleasedLeft` and `dualCasting` only with `castingLeft`, `castReleasedRight` only with `castingRight` (`validate.castFlags`, `IsValidQuantizedCheckpoint`); `shoutReleased` and the word flags only with `shouting`, at most one word flag, and `shoutFailed` only with `shoutReleased` (`validate.shoutFlags`, `IsValidQuantizedCheckpoint`) |
| `time` | `uint32`, the sender's `steady_clock` in milliseconds when sampled, modulo 16384 | `0..16383` |
| `skeleton` | `bytes`, the last field, since the server writes each recipient's pose change behind the encoded others ([Interest and delivery](interest-and-delivery.md#skeleton-poses)): the sender's skeleton pose ([below](#skeleton-pose-encoding)); empty when not sent | empty, or a layout below: whole (`validate.SkeletonPose`) or a [change](#pose-changes) that resolves against the stream's previous pose |

| `Context` field | Validation |
| --- | --- |
| `interior_cell`, `worldspace` | [form references](#form-references) of the interior cell the player is in, or of the worldspace outside; never both. `interior_cell` is unset in a worldspace, so crossing exterior cells changes no field. Both are unset in a space without a form reference |
| `location` | the player's current location (`BGSLocation`) as a [form reference](#form-references), unset for none; only named in the [player menu](ui-overlay.md#player-menu) |
| `graph_epoch` | `>= 1`, never decreasing while in the world |
| `furniture` | occupied placed reference as a [form reference](#form-references), unset for none |
| `shapeshift` | `NONE`, `WEREWOLF`, `VAMPIRE_LORD`, or `WEREBEAR`: the vanilla beast form the player is in ([Appearance and equipment](appearance-and-equipment.md#shapeshifts)) |
| `idle` | the idle event that entered the idle the player is held in ([Idle presentation](idle-presentation.md#state-and-actions)), empty for none; a held idle name ([Actions](#actions)), only without `furniture` and with `shapeshift` `NONE` |

- The held idle is a context field: it changes only when an idle starts or
  ends, so it costs the 20 Hz checkpoints nothing, and `PeerEnter` carries it
  to late joiners. Each change forces the next checkpoint out past idle
  suppression
  ([Interest and delivery](interest-and-delivery.md#rate-classes-and-idle-suppression)).
- Exterior cell IDs stay off the wire: nothing compares them, and each cell
  crossing would resend the context and relay a `PeerContext`.
- `protocol/Quantization.h` and `server/internal/wire/quantize.go` hold the
  same rules: positions round to quarter units (`kPositionScale`,
  `wire.PositionScale`, 4) and speed to whole units, heading is normalized to
  [0, 2π), pitch and look pitch are clamped, and direction wraps (1.0 is 0).
  Whole units changed a 50 ms walking segment's length by about 12%, which the
  exactly followed path showed as speed changes; quarter units cost about 4%
  more per relayed entry. The server reads positions in world units only
  through `wire.Position` and `wire.Units`.
- `time` is wall time, not accumulated frame time, so a paused or hitching
  source does not stretch it; receivers play checkpoints at its spacing
  ([Sync pipeline](sync-pipeline.md#checkpoint-playout)) and recover the wraps
  from arrival times. The server does not interpret it. Virtual players and
  `cmd/loadgen` bots stamp their own.
- Flag bits are ordered by how often they are set, so the movement state with
  `grounded` and `weaponDrawn` stays a one-byte varint on foot. The fly state
  and wing stroke ride in the flags rather than the context, since they must
  play in step with the checkpoints around them.
- The server accepts a `Context` or `Checkpoint` only in the world, and a
  checkpoint only after a context since the last `Enter` (`no context`); a
  context identical to the stored one is dropped.

Each field's meaning is in its owning system: movement in
[Locomotion](locomotion.md), stance and hands in
[Combat presentation](combat-presentation.md), furniture, mounts, and death in
[Native interactions](native-interactions.md), swimming in
[Remote actor](remote-actor.md#swimming).

### Skeleton pose encoding

A client sends `skeleton` only inside a pose window
([Remote actor](remote-actor.md#source-window)). A window is the run of
checkpoints that carry one; no message opens or closes it, so a late joiner
sees an open window at once. The server relays the pose only to recipients
that rank the sender among their nearest posed peers
([Interest and delivery](interest-and-delivery.md#skeleton-poses)).
`protocol/SkeletonPose.h` and `server/internal/validate` hold the same layout
of a whole pose:

1. a layout byte, `2` (`kSkeletonLayoutHuman`, `validate.SkeletonLayoutHuman`):
   the bone order of `Network::kSkeletonBoneNames`, 63 bones;
2. the mask of missing bones, bit `i` for bone `i`, as an unsigned varint in
   its shortest form, so `0` (every bone present, the usual case) is one byte;
   bit 63 and above must be clear;
3. the first bone's (`NPC COM`) local translation, three little-endian `int16`
   in 1/16 units;
4. the rotations of the present bones in bone order, bit-packed least
   significant bit first across bytes and padded with zero bits to a whole
   byte: per bone, the index of the quaternion's largest component in two
   bits, then the other three, each scaled from ±1/√2 to `0..2^b − 1` in `b`
   bits. The largest is made positive and rebuilt from the unit length.

`b` is 9 for bones 0–22 (COM through the hands), 8 for the twist bones
(23–30), `Weapon`, and `Shield`, and 7 for the fingers (31–60)
(`Network::GetSkeletonComponentBits`, `validate.SkeletonComponentBits`),
since an error in the trunk and limbs moves every bone below them. On 941
recorded poses these steps added at most 0.45° per bone in the trunk and limbs,
0.9° in the 8-bit bones, and 1.4° in fingers, against 267 bytes a pose at 10
bits throughout. Fingers move in these animations too, so they keep coarser
steps rather than being dropped.

The length must be exactly the header plus the packed rotations of the
present bones; a full pose is 211 bytes, and no pose exceeds 219
(`MaximumSkeletonPoseSize`, the nanopb `max_size`). Every bit pattern of a
rotation is a valid one, so nothing else is checked.

#### Pose changes

Each stream of poses carries most of them as a change from the stream's
previous pose, its reference: layout `3` (`kSkeletonLayoutHumanChange`,
`validate.SkeletonLayoutHumanChange`), written by
`Network::EncodeSkeletonPoseChange` and `wire.SkeletonPose.AppendChange`, and
read by `Network::ResolveSkeletonPose` and `wire.ResolveSkeletonPose`. A
change rebuilds exactly the quantized pose its sender had, so no error
accumulates; on a recorded window a change averaged 87 bytes against 211.

1. the layout byte `3`, then the mask of missing bones as in layout 2;
2. bit-packed as in layout 2: a 2-bit order `k`, then the three translation
   changes from the reference's;
3. per present bone in bone order: a bit, `0` for the reference's rotation
   unchanged; else a bit, `1` for the rotation written whole as in layout 2,
   `0` for a change: a 2-bit `k`, then the three stored components' changes,
   with the reference's largest-component index kept.

Changes are signed Exp-Golomb codes of order `k`: zigzag `v` to
`u = 2v` or `−2v − 1`, take `w = u + 2^k` of bit width `n`, and write
`n − 1 − k` zero bits, a one bit, then the low `n − 1` bits of `w`. A reader
refuses more than 20 zero bits. The encoder picks each `k` and each bone's
form by size, and sends the whole pose instead when the change would not be
smaller, so a change is never larger than its whole pose. A change is refused when it
reads past its end or leaves bytes or nonzero padding bits, when it keeps or
changes a bone the reference lacks, when a translation leaves `int16` or a
component leaves `0..2^b − 1`, when it is longer than 219 bytes, and when the
stream holds no reference.

A stream is one sender's poses in order on one connection, which delivers
every message in order, so both ends hold the same reference without
acknowledgements:

- **Client to server.** The reference is the pose this client last sent on the
  current socket (`ServerClient`'s `sentPose`, reset when the socket changes)
  and the pose its session last resolved (`session.Server.read`). The session
  resolves each checkpoint's pose before validating anything else, so a
  checkpoint rejected later still moves the reference, and replaces it with
  layout 2: the hub, recordings, and replays see only whole poses. An
  unresolvable pose drops its checkpoint and leaves no reference, so changes
  are refused until a whole pose.
- **Server to client.** The reference is the pose of the peer last written to
  this recipient: the outbox encodes posed entries per recipient as it drains
  them, in write order ([Interest and delivery](interest-and-delivery.md#skeleton-poses)).
  Both ends drop it at the peer's `PeerEnter` and `PeerLeave` and with the
  connection; a checkpoint without a pose leaves it. The client resolves each
  entry as it decodes the message, before anything may discard the
  checkpoint, and passes layout 2 on (`ServerClient::ResolveSkeletonLocked`).
  An unresolvable pose is dropped with the reference, and logged once until
  one resolves (`Dropped an unresolvable skeleton pose`). A server message
  that fails to decode drops every reference, since it may have carried
  poses.

Poses travel on every checkpoint of a window: at 10 Hz, Catmull-Rom
interpolation was off by 9° at the 95th percentile in the trunk and limbs.

## Actions

`Action` carries one attack, beast action, idle event, or power as a separate
ordered stream, since a checkpoint field would collapse repeated actions.

| Field | Validation |
| --- | --- |
| `action_sequence` | `>= 1`, exactly the previous plus one per player, shared by every kind; the first one a server sees sets the boundary |
| `graph_epoch` | equal to the player's context epoch |
| `checkpoint_sequence` | `>= 1`, a published checkpoint no newer than the latest |
| `kind` | a oneof, required: `attack`, `beast`, `idle`, or `power` |
| `attack.action` | `NORMAL`, `LEFT`, `DUAL`, or `BASH` |
| `attack.direction` | `NONE` for an attack that is not a power attack, else `STANDING`, `FORWARD`, `BACKWARD`, `LEFT`, `RIGHT` |
| `beast` | a `BeastAction`: the `WEREWOLF_` values `1..19` (also the werebear's) or the `VAMPIRE_LORD_` values `32..51`; any other value is rejected |
| `idle` | an idle graph event name ([Idle presentation](idle-presentation.md#capture)): a held idle name, or `OffsetStop` in any case |
| `power` | a [form reference](#form-references), required: the lesser or greater power the player cast ([Combat presentation](combat-presentation.md#shouts-and-powers)) |

A held idle name, in an action or the context, starts with `Idle` in any case
and continues with at least one more character, all from `[A-Za-z0-9_]`, 5 to
64 bytes in all (`Multiplayer::IsValidHeldIdleEvent`, Go
`validate.HeldIdleEvent`). Receivers send it to an actor's graph, so the
plugin also checks it when decoding a `PeerContext`, `PeerEnter`, or
`PeerAction` and when encoding. nanopb holds it in a 65-byte array and refuses
a longer string while decoding.

For an attack, the server also requires the latest checkpoint to have the
weapon drawn and the context to have no shapeshift. A beast action needs a
context whose form plays that graph (`validate.SameBeastGraph`: a werebear
plays the werewolf's), and not the weapon drawn, since a beast howls and feeds
sheathed. An idle event or a power needs a context without shapeshift, with the
weapon drawn or sheathed. Each needs a context without furniture, and the server allows
`limits.actions_per_second` (10, burst 10) per player. A duplicate, stale, or
gapped sequence is rejected rather than played out of order. Attacks and beast
actions keep graph event names out of the wire: the plugin maps each value to
its event. Idle events carry the name, because any vanilla idle from any
source counts and a viewer's graph decides whether it knows it. The
vocabularies' meaning is in
[Combat presentation](combat-presentation.md#action-stream),
[Combat presentation](combat-presentation.md#beast-actions), and
[Idle presentation](idle-presentation.md).

## Appearance and equipment

Appearance, equipment, and active effects are reconstructable state that
changes rarely, so each has its own
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
(or light index under `FE`), the index the receiver's `LookupFormID` rebuilds.
`TESForm::GetFile(0)` is not used: an overridden placed reference can list
only the overriding file (Skyrim.esm's `000DDFB3` lists `Update.esm`).

### `Appearance`

| Field | Validation |
| --- | --- |
| `name` | at most 128 UTF-8 bytes, no control characters (below U+0020, U+007F). The server replaces a client's with its [session's display name](#sessions) on the stored and relayed copy, and the client sends none; a replayed player keeps its recorded name |
| `level` | `0..65535`, `0` unknown |
| `race` | required form reference |
| `female` | boolean |
| `weight` | finite, `0..100` |
| `height` | finite, `0.25..4` |
| `head_parts` | at most 32, each with a form and any `type` |
| `hair_color` | RGB `0..255`, unset for none |
| `face_details` | form reference, unset for none |
| `body_tint` | required, RGB `0..255` |
| `face_morphs` | exactly 19 finite floats; unset morphs hold `FLT_MAX` |
| `face_parts` | exactly 4 `sint32`, each `-1..255` (`-1` for none); the plugin clamps to this range when capturing and parsing |
| `tints` | at most 64; `index` `0..65535`, `type` `0..14`, a color, `alpha` `0..1`, a [texture path](#texture-paths) |
| `overlays` | at most 32; `area` `BODY`, `HANDS`, `FEET`, or `FACE`; a [texture path](#texture-paths); `color` and `glow_color` RGB `0..255`, both required; `alpha` `0..1`; `glow_strength` `0..100` |
| `default_race` | a base-game race, unset for none; the fallback race keeps the appearance's `female` |

Hair color is RGB rather than a form, because character editors commonly
create dynamic color forms.

#### Texture paths

A tint's `texture` and an overlay's `texture` name a file the receiver opens,
so both ends accept only a relative `.dds` path (suffix compared without case)
of 1..260 bytes of printable ASCII (`0x20..0x7E`), without `:`, without a
leading `\` or `/`, and without a `..` component (`IsValidTexturePath`, Go
`texturePath`). The plugin drops a captured tint or overlay whose path fails
rather than the whole appearance.

### `Equipment`

| Field | Validation |
| --- | --- |
| `armor` | at most 32, unique references; `slot_mask` non-zero (bit 0 is slot 30) |
| `right_hand`, `left_hand` | a hand, unset for empty |
| hand `kind` | `WEAPON`, `TORCH` (left hand only), or `SPELL` (no weapon type; the spell or scroll as `form`, unset when unreferenceable) |
| hand `casting_art`, `casting_light` | only for `SPELL` and a staff (`WEAPON` with weapon type 8), each a form reference or unset: the hand look of the spell or of the staff's enchantment, used when the receiver lacks it ([Combat presentation](combat-presentation.md#spell-look)) |
| hand `enchantment` | only for a staff, a form reference or unset: the staff enchantment the staff casts, unset when it casts nothing or is unreferenceable ([Combat presentation](combat-presentation.md#staves)) |
| hand `weapon_type` | `WEAPON_TYPE` `1..9`, only for a weapon; two-handed types (5, 6, 7, 9) only in the right hand, which then requires an empty left hand |
| `ammo` | form reference, unset for none |
| `shout` | form reference, unset when the voice slot holds a power or nothing: the shout the checkpoint's shout flags present ([Combat presentation](combat-presentation.md#shouts-and-powers)) |
| `default_outfit` | unset is empty |
| `beast_right_hand`, `beast_left_hand` | a `SPELL` hand, unset for empty: a vampire lord's spells, while the fields above keep the character's items ([Appearance and equipment](appearance-and-equipment.md#shapeshifts)) |
| `beast_armor` | at most 8, as `armor`: the vampire lord's worn armor |
| outfit `armor` | at most 8, non-zero slot masks |
| outfit `hands` | at most 2, a weapon with a weapon type or a torch, no casting art, light, or enchantment, no two with the same kind and weapon type |
| outfit references and `default_race` | from a base-game plugin, compared without case |

`slot_mask` and `weapon_type` are hints that select the default outfit piece;
the receiver's locally resolved form decides how an item is equipped. The
base-game plugins are the five Special Edition masters and
`ccFSVSSE001-Backpacks.esl`, without other Creation Club plugins;
`Multiplayer::IsBaseGamePlugin` and the server's `baseGamePlugins` hold the
same list.

### `Effects`

The lasting effects that show on the player's actor
([Combat presentation](combat-presentation.md#active-effects)).

| Field | Validation |
| --- | --- |
| `effects` | at most 32 `ActiveEffect`, unique by `effect` (compared as sent) |
| `ActiveEffect.effect` | required form reference: the magic effect, not the item that applied it, which is dynamic for crafted potions and player enchantments |
| `ActiveEffect.magnitude` | finite, `-1e6..1e6` |

## Server messages

| Message | Fields |
| --- | --- |
| `Welcome` | `slot`, `session_id`, `display_name`, and `session_token` only when the session was issued for this connection ([Sessions](#sessions)); `voice`, a `VoiceAccess`, while the server carries voice ([Voice](#voice)) |
| `ChatResult` | `request_id` and `rejection`: `NONE` accepts the `ChatSend`, else `INVALID`, `RATE_LIMITED`, `NOT_IN_WORLD`, or `UNKNOWN_RECIPIENT` ([Chat](#chat)) |
| `Pong` | `request_id` of the `Ping` |
| `PeerStates` | `repeated Checkpoint` with `player` set; the concatenation of entries encoded once per sender |
| `PeerEnter` | `slot`, `appearance` and `equipment` when newer than what the recipient holds, `context`, `state` (the stored checkpoint). Effects are not a field: nanopb's static `PeerEnter`, about 63 KB with appearance and equipment, would exceed its 16-bit field limit without `PB_FIELD_32BIT` |
| `PeerContext` | `slot`, `context` |
| `PeerLeave` | `slot`, `reason`: `OUT_OF_RANGE`, `EXCLUDED`, `EXITED`, `DISCONNECTED`, `VIEWER_UNPLACED` |
| `PeerAction`, `PeerAppearance`, `PeerEquipment`, `PeerEffects` | `slot` and the message; `PeerEffects` also follows a `PeerEnter` when the recipient holds no newer revision |
| `ChatMessage` | `channel`, `sender`, `recipient` (whisper only), `text`; each a `PlayerIdentity{slot, name, session_id, role}`, `role` a `Role.id` or 0 |
| `Roster` | `joined`, a list of `PlayerIdentity`, and `left`, a slot or 0 ([Roster](#roster)) |
| `Roles` | `roles`, every `Role{id, name, color}` of the server, in place of those sent before ([Roles](#roles)) |
| `NearbyPlayers` | `joined` and `left`, packed slots entering and leaving the recipient's Nearby chat reach ([Nearby players](#nearby-players)) |
| `Inspection` | `slot`, `level`, `interior_cell`, `worldspace`, `location` of the player an `Inspect` named ([Inspection](#inspection)) |
| `TradeInvite`, `TradeOpen`, `TradeOffer`, `TradeReport`, `TradePluginQuery`, `TradePluginReply`, `TradeAccept`, `TradeCommit`, `TradeClose` | A trade's progress ([Trade](#trade)) |

When each is sent and in what order is
[Interest and delivery](interest-and-delivery.md#ordering).

## Server

- A player is in the world between `Enter` and `Exit`, and placed once it also
  has a context and a checkpoint. `Exit` forgets the context, checkpoint,
  sequences, and action stream; appearance, equipment, and effects are kept.
- A connection that closes, for any reason, is removed from the hub: its
  capture ends, and every recipient that received it or holds its presentation
  gets `PeerLeave` with `DISCONNECTED`.
- Messages are decoded and validated on the connection's reader goroutine and
  applied in arrival order by the hub. `Ping` is answered by the connection
  itself.
- The server reports each joined connection, why it ended, and each refusal
  to the metrics recorder, and counts the payload bytes it reads and writes
  ([Recording](metrics.md#recording)).

### Sessions

Goal: every connection belongs to a player the server can recognize again. A
session is that identity on one server: a public ID players can refer to
whatever the player is called, a secret only the player's client holds, and a
display name no other session has. Non-goals: accounts, passwords, recovering a
lost session, permissions.

| Part | |
| --- | --- |
| `session_id` | `uint64` from 1, an `AUTOINCREMENT` row ID, never reused. Sent in `Welcome`, on roster entries, and on chat speakers, so a client mutes or vanishes a player whatever slot or name it connects with. The hub keeps it as `world.Player.Session` |
| Token | 32 bytes from `crypto/rand`; the store keeps only its SHA-256, so a copy of the database resumes nobody. Returned once, in the `Welcome` of the connection it was issued for |
| Display name | 3..15 (`validate.MinimumDisplayNameBytes`, `validate.MaximumDisplayNameBytes`) ASCII letters, digits, and underscores (`validate.DisplayName`, `Multiplayer::IsValidDisplayName`); nanopb buffers of 16 bytes. Unique by `validate.DisplayNameKey`, its lower case; a session may respell its own |
| Last use | Set by every hello, when a connection closes, and hourly for connected sessions (`session.Server.Maintain`) |

- `auth.Store` keeps sessions in SQLite (`modernc.org/sqlite`) at
  `sessions.database` (`sessions.db`; empty keeps them in memory), with a
  write-ahead log and `user_version` 4; older databases are migrated in place
  when opened. Its one connection serializes transactions, which the display
  name checks rely on. Store calls run on a connection's goroutine, never the
  hub's.
- A session lives `sessions.lifetime_days` (30) after its last use, so one that
  keeps connecting never expires and none expires while connected. A banned
  session never expires ([Kicks and bans](#kicks-and-bans)). Expired
  sessions are deleted before every display name comparison and hourly, which
  frees their display names; their tokens then match nothing.
- The hello's `session_token` is empty or 32 bytes, else the connection is
  closed with 1008 `invalid session token`; a `display_name` that is not one is
  closed with 4005 `invalid display name`.
- A token of a live session resumes it, renaming it first to a different
  display name; one another session holds closes with 4004 `display name taken`
  and leaves the session as it was. Only the hello sets a display name, so it
  is fixed for a connection.
- No token, or one matching no live session, issues a new session under the
  display name, or closes with 4004 when it is held.
- A session issued for a connection that closes before the client sent anything
  after its hello is deleted (`Store.Abandon`), since the welcome may not have
  reached the client, which would then find its own name taken.
- A session is connected once. When it connects again, `Hub.ConnectClient`
  disconnects the earlier connection's player (peers get `PeerLeave` with
  `DISCONNECTED`) and closes its socket with 4006 `session connected
  elsewhere`, so a reconnect never waits for its dead connection's timeout.
  The takeover is decided on the hub goroutine, so racing connections end with
  one connected.
- A store failure closes the hello's connection with 1011
  `session store unavailable`.
- A new session is issued only when its address may take one
  ([Admission](#admission)); the check runs after the display name is found
  free, so a taken name does not spend the allowance.
- The admin API's `POST /api/sessions` `{displayName, token?}` runs the same
  resume-or-issue without the allowance, for `cmd/loadgen`
  ([Load testing](recording-and-replay.md#load-testing)). `token` is base64,
  empty or 32 bytes, and `displayName` passes `validate.DisplayName`, else 422;
  a held name answers 409, a banned session 403. It answers `{sessionId,
  displayName, token}`, 201 for a new session or 200 for a resumed one. Such a
  session is not abandoned; it expires.
- The admin API lists, renames, and deletes sessions
  ([Recording and replay](recording-and-replay.md#admin-api)).
  `session.Server.RenameSession` refuses a connected session
  (`session.ErrConnected`); otherwise `Store.Rename` applies the hello's
  uniqueness rule (`auth.ErrNoSession` for an expired or unknown one). The
  player's next hello renames it back to the name it carries.
  `session.Server.DeleteSession` deletes the session and its bans
  (`Store.Delete`) and closes its connection with 4011 `kicked`; its token then
  issues a new session with a new `session_id`, which ends friends, mutes, and
  vanishes other clients keep. Logs: `session renamed`, `session deleted`.
- A client is named by its session everywhere: `Appearance.name`,
  `PlayerIdentity.name`, the admin state, and a recording's header. Replays and
  in-server bots keep their recorded names, compared with no one's.

### Admission

Goal: bound what connections cost before a player is admitted, so a flood of
connections or hellos cannot crowd out real players or squat display names.
`admission.Gate` decides; `session.Server` asks it. Admin replays and
in-server bots do not pass through it and do not count.

- `limits.max_pending_hellos` (64) caps connections between the upgrade and
  the end of their hello, which bounds the hellos queued on the store's one
  connection. Over it, the upgrade answers HTTP 503 before a WebSocket exists.
- `limits.max_pending_hellos_per_address` (8; 0 no cap) caps the same per
  address (IPv6 per /64, as everywhere here), checked first with HTTP 429, so
  one host holding silent sockets cannot fill the shared cap. Eight leaves room
  for players behind one NAT joining together. A flood over many addresses is
  left to the proxy in front.
- `limits.max_players` (100; 0 no cap) caps connected sessions, which also
  bounds the roster. A hello without a token on a full server closes with 4007
  `server full` before the store is touched; otherwise the session is admitted
  after authenticating unless the server is full and it is not already
  connected (a session replacing its own connection is always admitted). A new
  session issued in the race is deleted again.
- `limits.max_players_per_address` (3; 0 no cap) caps sessions connected from
  one address, checked the same way after `max_players`, closing with 4009
  `too many players from address`; a session already connected from that
  address is admitted, one connected elsewhere is not. Players behind
  carrier-grade NAT share it with strangers; `cmd/loadgen` needs it off.
- `limits.new_sessions_per_address_per_month` (10; 0 no limit) is an in-memory
  token bucket per address, refilled at that many per 30 days, so a restart
  refills it. Resuming and the admin API never spend it. `Server.Maintain`
  prunes full buckets hourly.
- The address is the TCP peer's, unless the peer is in `trusted_proxies`
  (addresses or CIDR prefixes, empty by default); then it is the rightmost
  `X-Forwarded-For` hop not in `trusted_proxies`, since earlier hops may be
  forged. An unparsable hop stops the walk at the last trusted address. Behind
  an untrusted proxy every player shares the proxy's allowance.
- Upgrades refused for either hello cap are logged as a throttled summary
  (`refused connections`, with the cap as its reason), since a flood would
  repeat them.

### Kicks and bans

Goal: an admin can remove a connected player from the server, and keep one
out, from the admin API
([Recording and replay](recording-and-replay.md#admin-api)). Non-goals: timed
bans, a reason the player sees (clients show only that the connection
ended, [UI overlay](ui-overlay.md#notices)), banning a player who is not connected,
and stopping a player who changes both session and address.

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
  `Store.Lift` deletes a ban and restarts its session's lifetime. Deleting a
  session deletes its bans, address bans included.
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
- `WHISPER` names its recipient by slot from the [roster](#roster), so a
  whisper never reaches whoever holds the name after the recipient left. A slot
  that is not a connected client, replays and bots included, is `unknown
  recipient` (`UNKNOWN_RECIPIENT`). It goes to sender and recipient (once when
  the same).
- 5 accepted messages per player in any 5 s over all channels
  (`rate limited`, `RATE_LIMITED`); rejected messages do not count, and a disconnect forgets
  the history.
- Every `ChatSend` is answered with a `ChatResult` naming the rejection
  (`hub.chatRejection`) or `NONE`; the client picks its own text from the value.
- Every channel includes the sender, so everyone sees one order and the
  sender's line confirms delivery. A speaker is the player's `PlayerIdentity`.
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

Every client knows who is connected, so it can complete a name, list players
([UI overlay](ui-overlay.md#player-list)), and address a whisper by slot.
Proximity does not limit it.

- The roster lists the clients that have a session, by slot, session display
  name, session ID, and [role](#roles). Replays and in-server bots are not
  listed.
- `Hub.ConnectClient` sends the new client one `Roster` after its `Welcome`
  with every listed client in `joined`, itself included, and every other
  listed client a `Roster` with the new one in `joined`.
- A listed client that disconnects, also when its session connects again, is
  sent to every other listed client as `Roster{left}`. A display name changes
  only with a hello ([Sessions](#sessions)), so a rename is a leave and a
  join under another slot. A role changes while connected: the entry is sent
  again in `joined` ([Roles](#roles)).
- Roster messages are ordered with the rest of a client's outbox
  (`Outbox.AppendGlobal`) and marshalled once per event; the full list, 5 to 11
  bytes plus the name per client, is sent once per connection.
- The client rejects a `Roster` whose entry has no slot, no session ID, no
  name, or an invalid name (`ReadPlayerIdentity`, the rule for every
  `PlayerIdentity`), or with more than `kMaximumRosterEntries` (4096)
  entries.

### Roles

Goal: a server's admin marks players, such as moderators or supporters, so
everyone recognizes them in game. A role is a name and a color; a session holds
at most one, shown as a badge by the player's name
([UI overlay](ui-overlay.md#roles)), and made and given in the admin panel
([Admin API](recording-and-replay.md#admin-api)). Non-goals: permissions,
several roles per player, an order among roles, roles players pick, roles for
replays and bots.

| Part | |
| --- | --- |
| `Role.id` | `uint32` from 1, an `AUTOINCREMENT` row ID, so a deleted role's ID, which clients may still hold on chat lines, never names another role |
| `Role.name` | 1..24 characters (`validate.MaximumRoleNameRunes`) of valid UTF-8 without control characters or a space at either end (`validate.RoleName`). Unique among roles by `validate.RoleNameKey`, its lower case; a role may respell its own. A nanopb buffer of 97 bytes |
| `Role.color` | `0xRRGGBB` |
| Holder | `sessions.role_id`, `NULL` without a role |

- `auth.Store` keeps at most `validate.MaximumRoles` (32) roles
  (`ErrTooManyRoles`, `ErrRoleNameTaken`, `ErrNoRole`). `Store.Roles` lists
  them by ID with the number of live sessions holding each. `Store.SetRole`
  gives a live session a role in place of its own, or with 0 none;
  `Store.DeleteRole` leaves its holders without one in the same transaction.
- A role does not keep its session alive; an expired or deleted session's role
  goes with it.
- `Store.Authenticate` returns the session's role. The hub keeps it on the
  player (`world.Player.Role`) and sends it as `PlayerIdentity.role` wherever
  it names the player: roster entries, chat speakers, and trade invitations.
- `Roles` carries every role. `Hub.SetRoles` keeps the marshalled message,
  sends it to every client, and leaves a member whose role is not in it
  without one. A connecting client is sent it between its `Welcome` and its
  `Roster` while there is at least one role. `Hub.SetRole` sets the role of a
  session's connected player and sends every listed client, the player
  included, a `Roster` with the player's entry in `joined`, which replaces
  the entry the client has.
- `session.Server` changes roles (`CreateRole`, `UpdateRole`, `DeleteRole`,
  `SetSessionRole`): each writes the store and then the hub under one mutex,
  so the hub's roles follow the store's in order. `PublishRoles` gives a
  starting server's hub the stored roles. The server logs `role created`,
  `role changed`, `role deleted`, and `session role set`.
- The client rejects a `Roles` with more than `kMaximumRoles` (32) roles, a
  repeated ID, or a role that fails `Multiplayer::IsValidRole`: no ID, an
  empty or too long name, a control character, or a color above `0xFFFFFF`.
  A `PlayerIdentity.role` that names no role the client has is not an error
  and shows no badge, so a deleted role's badge goes from every chat line
  with the `Roles` that drops it.

### Nearby players

Every client knows which listed players its Nearby lines reach, so the
[player list](ui-overlay.md#player-list) shows them in the Nearby color. The
client cannot work it out: it receives at most `max_visible` peers within its
view radius, while Nearby reaches a whole interior.

- The set follows [Nearby chat](#chat) (`Hub.hearsNearby`): the listed
  players placed in the same space, in a worldspace within
  `chat.nearby_radius`, and in an interior all of it. The rule is mutual, and
  like Nearby chat it ignores exclusions, hiding, vanish, and the cap.
- `Hub.updateNearbyPlayers` runs after each interest pass and scans the listed
  clients due, each a second after its previous scan
  (`nearbyPlayersInterval`), so a crowd's scans spread out and a change shows
  up to about 1.2 s late; scanning every viewer every pass cost 500 bots 23% of
  relayed entries. A scan finds candidates in `nearbyGrid`, an `interest.Grid`
  with `chat.nearby_radius` cells where an interior shares one cell
  (`nearbyView`), and walks the previously sent slots only when one may have
  left.
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

A player's menu shows the player's level and place
([UI overlay](ui-overlay.md#player-menu)), and a client holds them only for
the players it receives. The server holds every connected player's
appearance and context, so it answers for the others.

- `Inspect{slot}` with a zero slot is dropped as `invalid inspect`.
- `Hub.inspect` answers the asking connection alone, from its global queue,
  with `Inspection{slot, level, interior_cell, worldspace, location}`, the
  level from the named player's stored `Appearance` and the rest from its
  stored `Context`. A slot that is not a listed client ([Roster](#roster))
  gets `Inspection{slot}` alone, so the client stops waiting; a player
  without an appearance yet leaves `level` zero, and one outside the world
  (`Exit` clears the context) the place fields unset. Asking
  about oneself is answered too.
- Each connection may ask twice a second with a burst of 5
  (`inspectsPerSecond`, `inspectBurst`, a `limit.TokenBucket`); one more is
  dropped as `rate limited` without a reply. A reply is small and goes only
  to the asker, so the limit guards the hub's work rather than anyone's
  bandwidth.
- Neither is recorded. Vanish and hiding do not change the reply, since the
  name is on the roster anyway; the menu is the only reader.

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

### Voice

A connection on a server with `voice.enabled` (default) has a voice endpoint: a
random nonzero 32-bit ID and a random AES-256-GCM key for that connection alone
([Voice chat](voice-chat.md)). Frames travel as UDP datagrams to and from
`voice.listen` (`0.0.0.0:14201` by default), never over the WebSocket; who
receives them is in
[Interest and delivery](interest-and-delivery.md#voice-listeners).

`Welcome.voice` is a `VoiceAccess`:

| Field | Meaning | Client validation |
| --- | --- | --- |
| `host` | where to send datagrams, `voice.public_host`; empty for the server address's host, which the WebSocket connected to | at most 253 bytes of printable ASCII without spaces, `/`, or `\` (`Multiplayer::IsValidVoiceHost`) |
| `port` | `voice.public_port`, or the listening port | `1..65535` |
| `id` | the endpoint's ID | nonzero |
| `key` | the endpoint's key | 32 bytes |
| `radius` | `voice.radius`, toward which clients fade a speaker out | `100..65535` |

A welcome whose access fails validation is kept without voice, and the
plugin logs the fields. The server checks its configuration the same way:
`voice.radius` within `100..65535`, `voice.public_port` within `0..65535`, and
`voice.public_host` a host without a port, path, or whitespace.

Datagrams (`server/internal/voice/datagram.go`, `network/VoiceTransport`):

| Direction | Clear header, authenticated | Encrypted |
| --- | --- | --- |
| client to server | kind (1), ID (4, little-endian), counter (4) | voice: sequence (2), flags (1), Opus; probe: probe ID (4) |
| server to client | kind (1), counter (4) | voice: speaker slot (4), sequence (2), flags (1), Opus; probe: the probe's ID (4) |

- Kind 1 is voice and 2 a probe; flag bit 0 is `end`, and other bits are
  rejected. Each datagram ends with GCM's 16-byte tag over the header and the
  ciphertext. The 12-byte nonce is a direction byte (1 to the server, 2 to the
  client), three zero bytes, the counter, and four zero bytes; each side's
  counter rises with every datagram it sends under the key, so a nonce never
  repeats.
- The relay finds the endpoint by ID, opens the datagram, and accepts its
  counter once and within the 64 below the highest seen (the same window on the
  client). A datagram that fails any check, or is over 432 bytes (`voice.MaximumDatagramBytes`), is dropped
  and counted, without a reply. Only the newest counter so far moves the
  endpoint's address, so a client that changes address, as behind a NAT that
  rebinds, is followed, and a late datagram from its old address does not take
  it back.
- A probe is answered at once to the address it came from. A voice datagram's
  frame is forwarded to the speaker's listeners.
- The relay sends a listener datagrams only once an authentic datagram, such
  as its first probe, gave its address; until then the listener is sent nothing
  ([Interest and delivery](interest-and-delivery.md#voice-listeners)).
- A datagram's frame is checked by `validate.VoiceFrame`: `sequence` within
  `0..65535` and `opus` of 1 to 400 bytes. The Opus data is the receivers' to
  decode; a decoder that fails plays silence.
- A speaker may send `voice.packets_per_second` (60, burst `voice.packet_burst`
  25) frames; a client sends 50 while talking. Frames over it are dropped
  without closing anything.
- Voice never reaches the hub, so it is neither ordered with the hub's messages
  nor recorded ([Recording and replay](recording-and-replay.md)).
- The client rejects a voice datagram with a speaker of 0, unknown flags, or
  `opus` empty or over 400 bytes.
- The endpoint ends with the connection; a datagram for it afterwards is an
  unknown ID.

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
- Only the newest pending checkpoint (by sequence), appearance, equipment, and
  effects (by revision) per peer are kept until the pass drains them. A checkpoint is
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
  The [roles](#roles) are kept with it (`Multiplayer::Roster::roles`):
  `Roles` replaces them, a join for a known slot also takes the entry's
  role, a welcome and a disconnect empty them, and each advances the same
  revision.
- The transport keeps the [nearby players](#nearby-players) sorted, with the
  roster under its lock: `NearbyPlayers` adds and removes slots, a roster's
  `left` removes one, and a welcome and a disconnect empty them.
  `Network::CopyNearbyPlayers` appends them; chat orders its name list and
  colors its player list by them.
- `Network::DescribeServer` gives `the official server` or `a custom server`.
  Every log line naming a server goes through it, so players can share logs
  without server addresses.
- A `Welcome` with voice access starts `VoiceTransport` for it
  ([Voice chat](voice-chat.md#relay-link)); one without it, and every lost
  connection, stops it. Received frames go straight to the voice sink on the
  datagram thread, outside the remote changes the pass drains.
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
  or with both `interior_cell` and `worldspace` or an invalid place
  reference is rejected, as a `Context` with them would be. The
  newest per slot is queued with the remote changes, drained as
  `RemotePlayerChanges::inspections`, and dropped with them when the
  connection is lost.

### Session

- `SessionStore` keeps one token per server in `PlayerHub.session`, next to
  `PlayerHub.ini`: a line of 64 hexadecimal digits, a space, and the WebSocket
  URL; `;` starts a comment. It is a file of its own because players share
  their ini and modpacks, and a token is all it takes to connect as its owner.
  Read once on first use, written through `PlayerHub.session.tmp`. Tokens are
  never logged.
- When the socket opens, the transport thread sends the hello with the token
  kept for the attempt's URL, or none, and `[Connection] DisplayName`.
- A `Welcome` with a token saves it, also when the connection is already being
  closed, since the session exists on the server either way. A failed save is
  logged as an error: the token lasts until the game exits.
- The welcome's session ID and display name are only logged; the roster
  carries the local player's session ID too.

### Player filters

- `PlayerFilterStore` keeps the local player's friends and the players it
  muted or vanished in `PlayerHub.players`: a tab-separated line per player and
  server with any of `f`, `m`, and `v` in that order, the session ID, the
  WebSocket URL, and the name when last seen ([UI overlay](ui-overlay.md#friends));
  `;` starts a comment, and a malformed line is skipped with a warning. Read
  once on first use, written through `PlayerHub.players.tmp` on every change.
  A player with no flag has no line.
- The hello carries the sessions vanished on the attempt's URL, at most 1024
  with a warning for the rest. `Network::PublishVanish` sends a change only
  when the current socket's hello went to the server the change belongs to,
  under the lock the hello is sent under: a `Vanish` before the hello would
  end the connection, and the hello reads the file, which the caller updates
  before publishing, so a change made meanwhile is in one or the other.
- `Network::GetServerUrl` is the URL of the latest welcomed connection, or
  of the configured server address before the first, or empty without one: the
  server whose session IDs the roster and chat lines carry. `PlayerFilters`
  reads the file for it again whenever the connection generation changes
  ([UI overlay](ui-overlay.md#mute-and-vanish)), and changes nothing while it
  is empty.

### Latest-wins publishers

Every welcome advances the connection generation. Appearance, equipment, and
effects republish on each new generation, since the server forgets a dropped
connection's state; `PresenceSync` sends `Enter` again, and the next checkpoint
its context. Remote players are rebuilt from `PeerEnter`.

### Connection

A connection is wanted only in a game, while the player's choice enables it,
once `[Connection] DisplayName` is set
([Appearance and equipment](appearance-and-equipment.md#display-name)), and
while `Config::GetServerAddress` gives an address or a connection is welcomed.
It gives none only with `CustomServer` on and no `ServerAddress`
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
  (default `true`) if a display name and a server address are set, and disabled
  otherwise. The settings window's Connect, Cancel, and Disconnect change it with
  `Network::SetConnectionEnabled` until the game ends.
- Applying a display name never opens a connection; only the choice does. The
  monitor reads the display name when the configuration revision changes, and
  one that becomes empty disables the choice, so entering one again waits for
  Connect. The server requires one too, in the hello.
- Applying a server address or switching `CustomServer` or `ServerTls` never
  touches the connection, so changing the setting does not drop players from
  the server they are on; each later attempt reads `Config::GetServerAddress`
  when it begins. A missing address while no connection is welcomed disables
  the choice and closes the client.
- A connection keeps the display name its hello carried; the settings window
  locks the field while a connection is wanted or open
  ([UI overlay](ui-overlay.md#connection-controls)).
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
- A close with status 4001 or 4004 through 4011 is a refusal: retrying cannot
  help, so the monitor disables the choice and closes the client.
  `Network::GetConnectionRefusal` returns its `Refusal{reason}` for the
  [connection controls](ui-overlay.md#connection-controls) until Connect or a
  game start or end; nothing retries by itself. 4006 is a refusal because two
  games holding one token would otherwise take the session from each other
  without end.

The monitor's timers accumulate the clamped frame delta, which is zero in the
main menu and while paused, so they hold while opening and closing still follow
the game.

### Vanilla intro hold

Goal: a player whose new game starts with the vanilla prisoner intro (cart
ride, execution, Alduin's arrival) is not put on a server while unable to move;
multiplayer starts once the player gets control back after Alduin's knockdown.
Non-goals: holding the connection in other scenes; a mod's replacement start,
including one that keeps MQ101 running or visits Helgen's attack; a cart ride
begun later in the play-through (Alternate Perspective), since the player has
already been online.

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
- During the hold the settings window cannot connect, its disabled button
  telling why ([UI overlay](ui-overlay.md#connection-controls)), the
  [display name hint](ui-overlay.md#display-name-hint) stays hidden, and the
  rest of the overlay behaves as while disconnected.

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
the wire (`wireSize`), in each direction, and each voice datagram with 28 bytes
of IP and UDP headers. TLS, TCP, and ping frames are not counted. Rates are averaged over the latest complete one-second wall-clock
window, and are empty after a long gap. The status panel shows them.

## Engine assumptions

- IXWebSocket's thread stops cleanly inside the game process at exit; only
  starting it and closing a connection on request were observed.

## Acceptance criteria

1. Two clients see each other move, attack, sit, ride, die, and change
   appearance, equipment, and lasting effects.
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

18. A role given to a connected player in the admin panel shows for every
    connected player, themselves included, without reconnecting: its letter
    mark before their name on chat lines, in the player list, and on their
    nameplate, and its whole badge in their player menu. Taking it, changing
    its name or color, and deleting it show the same way.
19. A player given a role while offline shows it from their next connection,
    and a session that expired or was deleted comes back without it.

## Known limitations

- A role set in the instant between a connection's hello and its join in the
  hub shows from the player's next connection.
- Sessions authenticate players only. The admin API is served only on
  `admin_listen` and is guarded by its own password
  ([Admin login](recording-and-replay.md#admin-login)), or by nothing but its
  refusal of browser requests from other sites when none is set.
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
  besides `_`. The narrow rule keeps names typeable after `@`, renderable by
  every font, and comparable without Unicode case folding.
- The token is sent in the hello as it is, so a server address without TLS exposes it
  to the network; with `ServerTls` or `server_tls` it travels under TLS.
- The official URL is kept out of the settings window and the log only: it is
  a plain string in the DLL and the key of its lines in the client's files,
  and IXWebSocket's reason for a failed connection, logged as given, may name
  the host.
- Admin exclusions and hiding are still keyed by slot
  ([Interest and delivery](interest-and-delivery.md#admin-controls)); only a
  player's own vanish follows the session.
- Chat has no history for late joiners.
- The roster tells every client the display name of every connected player,
  wherever they are, and its session ID. It carries no level.
- Form references, spaces included, match only between peers that load the
  defining plugin under the same file name: a merged or renamed plugin is
  another plugin.
- An action the server drops for exceeding `limits.actions_per_second` does not
  advance the player's action sequence, so the next one leaves a gap and is
  rejected, as is every later action until `Exit`. The plugin keeps its own
  idle events well under the allowance
  ([Idle presentation](idle-presentation.md#capture)).
- Non-goals: server authority over movement, anti-cheat, and UDP for anything
  but voice, whose frames lose their worth when TCP holds them back behind a lost
  segment, while every other message must arrive in order.
