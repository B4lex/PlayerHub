# Network

## Status

Implemented. The server's validation, relay, batching, late join, chat, and
latency probe are covered by unit tests in `server/tests`: `test_protocol.py`
for parsing, `test_relay.py` for batching, and one `test_app_<feature>.py` per
handler group (for example `test_app_presence.py`, `test_app_actions.py`,
`test_app_appearance.py`, `test_app_equipment.py`, `test_app_chat.py`,
`test_app_latency.py`). The wire carries
every feature that runs in game on Skyrim AE 1.7.104. The connection policy
and recovery ([Connection](#connection)) have not been checked individually in
game: the main menu staying disconnected, `AutoConnect = false`, recovery after
a server restart, a kick, or a blocked route, and the 3-second close delay.

## Scope

This system owns the wire protocol, the client transport and connection
management, and the server's validation, storage, and relay. It validates
untrusted fields at the boundary and queues plain values for the
[sync pipeline](sync-pipeline.md#receive-state), which applies them on the game
thread. Recording and replay are in [Recording and replay](recording-and-replay.md).

Protocol and server versions ship together with each client release, so the
protocol evolves directly, without compatibility shims. Recordings made before
a field existed replay with its documented default.

## Components

| Module | Responsibility |
| --- | --- |
| `src/protocol/` | Plain wire types and shared validation: `PlayerState.h`, `MovementState.h`, `AppearanceState.h`, `EquipmentState.h`, `FormReference.h`, `ChatMessage.h`. No Skyrim headers |
| `network/SocketIOClient` | socket.io-client-cpp 3.1.0 transport: emits, parses and validates received events, and queues them per player for `DrainRemotePlayerChanges` |
| `network/ConnectionMonitor` | When to connect, reconnect schedule, latency probes, and the connection state |
| `network/TrafficMeter`, `PacketSize` | Estimated bytes per second in each direction |
| `server/src/server/app.py` | FastAPI and python-socketio handlers |
| `server/.../protocol.py` | Payload validation |
| `server/.../registry.py` | `PlayerRegistry`: latest state per connection, world presence, name indexes |
| `server/.../relay.py` | `CheckpointRelay`: batched checkpoint broadcast |
| `server/.../chat.py` | `ChatThrottle` |

## Events

| Event | Direction | Payload and handling |
| --- | --- | --- |
| `session:welcome` | server → client | `{playerId}`, the connection's own ID, sent first on connect |
| `player:enter` | client → server | The player is placed in the world; not relayed |
| `player:exit` | both | The player left the world (loading screen, main menu, save load); relayed with `playerId` |
| `player:update` | client → server | One checkpoint, below |
| `player:states` | server → every client | `{states: [...]}`, the batched checkpoints, each with `playerId` |
| `animation:action` | client → server | One ordered attack action, below |
| `player:action` | server → peers | The action with `playerId` |
| `player:appearance` | both | Latest-wins appearance; relayed with `playerId` and `nameIndex` |
| `player:equipment` | both | Latest-wins equipment; relayed with `playerId` |
| `player:disconnect` | server → peers | `{playerId}`: forget everything about the player |
| `chat:send` | client → server | `{text}`, acknowledged |
| `chat:message` | server → every client | `{playerId, name, nameIndex, level, text}` |
| `latency:probe` | client → server | No arguments; acknowledged with `{"ok": true}` |

Every client event is acknowledged with `{"ok": true}` or
`{"ok": false, "error": ...}`. Graph event names never cross the network.

## Checkpoint

`player:update` is the latest-wins player state, sampled once per 0.05 s
([Sync pipeline](sync-pipeline.md#local-publishing)). Every relayed checkpoint
carries every field, and the client requires them.

| Field | Meaning | Validation |
| --- | --- | --- |
| `x`, `y`, `z` | Position of the carrier: the player, or its mount while `mounted` | finite |
| `heading` | Body angle | finite |
| `pitch` | Front-to-back tilt of the carrier's controller (`pitchAngle`), radians | finite, `|pitch| <= π/2`; missing is `0` |
| `speed` | Measured sample-to-sample speed | finite, `>= 0` |
| `direction` | Travel direction relative to heading, graph `Direction` units | `0..1` |
| `movementState` | `idle`, `walking`, `running`, `sprinting`, `sneakingIdle`, `sneakingWalking`, `sneakingRunning` | enum |
| `jumping` | Airborne jump phase | boolean |
| `weaponDrawn` | Weapon stance | boolean |
| `bowDrawn`, `castingLeft`, `castingRight`, `blocking` | Hand presentation | boolean; missing is `false` |
| `dead` | Dying or dead | boolean; missing is `false` |
| `mounted` | Riding | boolean; missing is `false` |
| `swimming` | The carrier swims | boolean; missing is `false` |
| `cellId`, `worldspaceId` | Coordinate space; worldspace `0` is an interior | `0..2^32-1` |
| `furniture` | Occupied placed reference as a [form reference](#form-references), `null` for none | form reference or `null`; missing is `null` |
| `graphEpoch` | Source graph continuity for actions | `1..2^63-1`, never decreasing |
| `sequence` | Checkpoint order | `0..2^63-1`, strictly increasing |

The meaning of each field is in its owning system: movement and locomotion in
[Locomotion](locomotion.md), stance and hand presentation in
[Combat presentation](combat-presentation.md), furniture, mounts, and death in
[Native interactions](native-interactions.md), and swimming in
[Remote actor](remote-actor.md#swimming).

Cell and worldspace IDs are load-order FormIDs and need the same plugins in
the same order on every peer. Furniture is plugin-qualified, so modded furniture
is shared by peers that load its plugin at any index.

## Actions

`animation:action` carries one attack as a separate ordered stream, since a
checkpoint field would collapse repeated attacks:

```json
{
  "actionSequence": 12,
  "graphEpoch": 3,
  "checkpointSequence": 4210,
  "family": "attack",
  "action": "normal",
  "power": true,
  "direction": "forward",
  "prerequisites": { "weaponDrawn": true }
}
```

| Field | Validation |
| --- | --- |
| `actionSequence` | `1..2^63-1`, exactly the previous plus one per player |
| `graphEpoch` | equal to the player's latest checkpoint epoch |
| `checkpointSequence` | a published checkpoint no newer than the latest |
| `family` | `attack` |
| `action` | `normal`, `left`, `dual`, or `bash` |
| `power` | boolean, required |
| `direction` | `standing`, `forward`, `backward`, `left`, `right`; required when `power` is true, rejected otherwise |
| `prerequisites` | exactly `{"weaponDrawn": true}` |

The server also requires the latest checkpoint to have the weapon drawn and no
furniture. A duplicate, stale, or gapped sequence is rejected rather than played
out of order; the first sequence a server sees sets the boundary, and no
historical actions are kept. Actions recorded before `power` existed no longer
replay. The vocabulary's meaning is in
[Combat presentation](combat-presentation.md#action-stream).

## Appearance and equipment

Both are reconstructable state that changes rarely, so each has its own
latest-wins event with a source-local `revision` (`1..2^63-1`, strictly
increasing per connection) rather than riding in every checkpoint. Capture,
resolution, and fallbacks are in
[Appearance and equipment](appearance-and-equipment.md).

### Form references

Items, races, head parts, and checkpoint furniture are `{plugin, formId}`: the
file that defines the form and its file-local ID (the low 24 bits, or 12 for a
light plugin). They do
not need matching load orders; a receiver resolves by plugin name. Plugin names
are 1..260 characters and `formId` is `0..0xFFFFFF`. Dynamic (`FF`) forms
cannot be referenced and are omitted.

### `player:appearance`

```json
{
  "revision": 3,
  "name": "Lydia Stormblade",
  "level": 12,
  "race": { "plugin": "Skyrim.esm", "formId": 79686 },
  "female": false,
  "weight": 50.0,
  "height": 1.0,
  "headParts": [{ "plugin": "Skyrim.esm", "formId": 324286, "type": 1 }],
  "hairColor": { "r": 59, "g": 42, "b": 30 },
  "faceDetails": null,
  "bodyTint": { "r": 222, "g": 188, "b": 170 },
  "faceMorphs": [0.0, "... 19 numbers"],
  "faceParts": [0, 0, 0, 0],
  "tints": [{ "index": 44, "type": 6, "r": 222, "g": 188, "b": 170, "alpha": 1.0 }],
  "defaultRace": null
}
```

| Field | Validation |
| --- | --- |
| `name` | at most 128 UTF-8 bytes, no control characters (below U+0020, U+007F); missing or `null` is empty |
| `level` | `0..65535`, `0` unknown; missing or `null` is `0` |
| `race` | form reference |
| `female` | boolean |
| `weight` | `0..100` |
| `height` | `0.25..4` |
| `headParts` | at most 16; `type` `0..6` |
| `hairColor` | RGB `0..255`, or `null`; required key |
| `faceDetails` | form reference or `null`; required key |
| `bodyTint` | RGB `0..255` |
| `faceMorphs` | exactly 19 finite numbers within the float range |
| `faceParts` | exactly 4 signed 32-bit integers |
| `tints` | at most 64; `index` `0..65535`, `type` `0..14`, `alpha` `0..1` |
| `defaultRace` | `{race, female}` with a base-game race, or `null`; missing is `null` |

Hair color is RGB rather than a form, because character editors commonly
create dynamic color forms. Unset face morphs hold `FLT_MAX`; the client's JSON
parser is not full precision and can decode it marginally above, so the client
clamps finite morphs to the float range instead of rejecting the payload. A
JSON `null` is accepted as a null message or an absent value.

The relayed appearance adds `nameIndex` ([Name index](#name-index)).

### `player:equipment`

```json
{
  "revision": 7,
  "armor": [{ "plugin": "Skyrim.esm", "formId": 77385, "slotMask": 4 }],
  "rightHand": { "plugin": "Skyrim.esm", "formId": 77936, "kind": "weapon", "weaponType": 1 },
  "leftHand": { "kind": "spell" },
  "ammo": { "plugin": "Skyrim.esm", "formId": 118996 },
  "defaultOutfit": { "armor": [], "hands": [], "ammo": null }
}
```

| Field | Validation |
| --- | --- |
| `armor` | at most 32, unique references; `slotMask` non-zero 32-bit (bit 0 is slot 30) |
| `rightHand`, `leftHand` | required keys; a hand entry or `null` |
| hand `kind` | `weapon`, `torch` (left hand only), or `spell` (no reference) |
| hand `weaponType` | `WEAPON_TYPE` `1..9`, only for `weapon`; two-handed types (5, 6, 7, 9) only in the right hand, which then requires an empty left hand |
| `ammo` | form reference or `null`; missing is `null` |
| `defaultOutfit` | missing or `null` is empty; the server always relays one |
| outfit `armor` | at most 8, non-zero slot masks |
| outfit `hands` | at most 2, `weapon` with a weapon type or `torch`, no two with the same kind and weapon type |
| outfit references and `defaultRace.race` | from a base-game plugin, compared without case |

`slotMask` and `weaponType` are hints that select the default outfit piece; the
receiver's locally resolved form decides how an item is equipped. The base-game
plugins are the Special Edition masters (`Skyrim.esm`, `Update.esm`,
`Dawnguard.esm`, `HearthFires.esm`, `Dragonborn.esm`) and the 74 Creation Club
plugins the Anniversary Edition `Skyrim.ccc` lists; `Multiplayer::IsBaseGamePlugin`
and the server's `BASE_GAME_PLUGINS` hold the same list.

## Server

- `connect` registers the connection, sends `session:welcome`, then every
  stored appearance and equipment of the other connections, so late joiners do
  not wait for a change. The namespace `CONNECT` is sent before the handler
  runs (`always_connect=True`), since the client discards events that reach it
  before the namespace opens.
- A player is in the world between `player:enter` and `player:exit`. A
  checkpoint outside it is rejected (`not in world`). `player:exit` drops the
  stored checkpoint and action sequence, discards an unsent checkpoint, and is
  relayed to peers; appearance and equipment are kept.
- `player:update` is rejected when malformed, when its sequence does not rise,
  or when its graph epoch falls. An accepted one is stored and handed to the
  relay with only the validated fields, so extra client fields such as
  `sentAtMs` are neither stored nor relayed.
- `animation:action` is validated against the latest stored checkpoint, as
  above. The player's unsent checkpoint is flushed first, so the checkpoint an
  action names is never overtaken, then `player:action` goes to the peers.
- `player:appearance` and `player:equipment` are rejected when malformed or
  when the revision does not rise; accepted ones are stored and relayed to the
  peers.
- `disconnect` stops a capture of the player, forgets its state, name index,
  and chat history, discards its unsent checkpoint, and relays
  `player:disconnect`.
- The server pings every client every 5 s and disconnects one that has not
  answered within 10 s.

### Checkpoint relay

`CheckpointRelay` keeps the newest unsent checkpoint of every player, and a
newer one replaces it. Every 0.025 s, twice the checkpoint rate, it broadcasts
them together as one `player:states`. The tick follows a fixed deadline, so the
flush time does not stretch it, and after a stall it restarts from now instead
of sending catch-up bursts. A broadcast without per-recipient exclusions is
encoded once for all clients, so each batch also reaches the players it
contains; the client skips its own entry by the ID from `session:welcome`.

### Name index

`PlayerRegistry.set_appearance` gives a player an index when its first
appearance arrives or its name changes. Names compare case-insensitively. The
player gets the lowest index from 1 not held by another stored player with that
name, and keeps it while the name stays the same, so indexes are never
renumbered when someone leaves. Replayed and load-test players are indexed the
same way. The index is relayed but not stored in the appearance, so recordings
do not contain it. [Appearance and equipment](appearance-and-equipment.md#player-label)
shows it in the label.

### Chat

- The server trims surrounding spaces and rejects empty text, control
  characters, unencodable strings such as lone surrogates, and more than 512
  UTF-8 bytes (`invalid chat message`).
- `ChatThrottle` allows 5 messages per player in any 5 s (`rate limited`).
  Rejected messages do not count, and a disconnect forgets the history.
- `chat:message` goes to every client, the sender included, so everyone sees
  one order and the sender's own line confirms delivery. `name`, `nameIndex`,
  and `level` come from the sender's latest appearance (empty, `1`, and `0`
  before one).
- Chat does not require being in the world, keeps no history, and is not
  recorded. The client side is in [UI overlay](ui-overlay.md#chat).

## Client

- Callbacks run on the transport thread. They validate each payload as the
  server does, log a rejected payload with the offending field, and queue plain
  values; they never touch game objects.
- Per player, only the newest pending checkpoint (by sequence), appearance, and
  equipment (by revision) are kept until the pass drains them, so a late older
  packet cannot replace a newer one. A checkpoint whose graph epoch falls is
  rejected. A batch is bounded to 4096 entries. Actions are queued in order.
  `player:disconnect` drops everything pending for the player.
- `chat:message` payloads are queued, at most 128, dropping the oldest, and
  kept across a lost connection.

### Latest-wins publishers

Every opened namespace advances the connection generation. Appearance and
equipment republish their last state on each new generation, because the server
forgets a connection's state when it drops; `PresenceSync` sends `player:enter`
again. Remote players are then rebuilt from what the server sends.

### Connection

A connection is wanted only in a game, and while the player's choice enables
it. `ConnectionMonitor` owns that decision; nothing else opens or closes the
client.

- At `kDataLoaded`, `Network::Initialize` only registers listeners; the main
  menu never connects.
- A game starts when `PresenceSync` first finds the player in the world, and
  ends when the main menu opens (`GameSession`). Loading screens, including
  loading another save from in game, keep the game and its connection.
- At a game start the choice becomes `[Network] AutoConnect` (default `true`).
  The settings window's Connect and Disconnect buttons change it with
  `Network::SetConnectionEnabled` until the game ends.
- Opening skips the reconnect delay and resets the attempt count. Closing
  (`BeginClose`) marks the client disconnected at once, forgetting every remote
  player, and runs `sync_close` on a worker. It shares the worker slot with
  `BeginReconnect`, so a busy attempt keeps the monitor in its closing phase,
  retried each pass. A namespace that still opens after a close was requested is
  ignored until the next `BeginReconnect`.
- socket.io-client-cpp 3.1.0 closes a connected namespace by sending
  `DISCONNECT` and waiting 3 s for a reply that a v4 server never sends
  (`socket::impl::close`). The
  transport closes within milliseconds, but `sync_close` returns only after that
  timer, so a connection requested within 3 s of a close starts when it expires.
- `Network::GetConnectionState` never runs ahead of the transport: `kConnected`
  while the namespace is open, `kConnecting` while one is wanted but not open,
  `kDisconnecting` while none is wanted but the transport is open, and
  `kDisconnected` otherwise.
- An applied endpoint that differs from the latest attempt's abandons a live
  connection and reconnects at once, with the attempt count reset. Each attempt
  reads the endpoint, and logs name it.

The monitor's timers accumulate the clamped frame delta, which is zero in the
main menu and while paused, so they hold while opening and closing still follow
the game.

### Loss and recovery

The client is disconnected exactly when the default namespace is not open, and
every loss while a connection is wanted is recoverable.

- A server restart, an Engine.IO ping timeout, or a namespace `DISCONNECT`
  reaches the client as a namespace close, a transport close, or a failed
  connect. Each marks the client disconnected and forgets every remote player.
- The library does not notice a server that stops answering. A latency probe
  without an ack for 5 s of accumulated frame time makes the monitor call
  `AbandonConnection`, which acts like a reported close.
- The library's own retry is disabled (`set_reconnect_attempts(0)`): it skips a
  close with a normal status, which is how a server drops a client, and leaves
  the namespace closed on a live transport. Each loss is reported once, and the
  monitor reopens.
- After a loss the monitor waits 1 s, then calls `BeginReconnect`, doubling the
  wait after each failed attempt up to 15 s, each wait scaled by a random factor
  in 0.5–1.5 so clients that lost one server do not retry together. Attempts are
  unlimited. An attempt reported failed, or not open after 20 s, is followed by
  the next.
- `BeginReconnect` runs on a worker: `sync_close` joins the old transport
  thread, so nothing from the dead connection is reported afterwards, then the
  handlers are registered again, since the library drops the socket and its
  handlers when the server closes the namespace, and `connect` is called.

### Latency

The client sends `latency:probe` 1 s after the previous one was sent, with at
most one outstanding. The round trip is measured with `std::chrono::steady_clock`
from emit to ack, so frame rate does not affect it. Each probe has an ID, and a
compare-exchange on the outstanding ID accepts only the current probe's ack;
acks from abandoned probes or older connections are ignored. A disconnect or new
generation clears the sample. The server does not relay or store anything for a
probe.

### Traffic

`TrafficMeter` counts the estimated Engine.IO size of every event and
acknowledgement in each direction (`PacketSize`, which models the server's
ASCII-escaped JSON and the client's UTF-8 JSON). WebSocket framing, TLS, and
heartbeats are invisible to the client and not counted. Rates are averaged over
the latest complete one-second wall-clock window, and are empty after a long
gap. The status panel shows them.

## Known limitations

- There is no authentication. The admin API can delete recordings and spawn
  virtual players, so the server port must not face the Internet.
- Chat has no history for late joiners.
