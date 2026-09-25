# Skyrim Multiplayer server

Local FastAPI and Socket.IO synchronization server for the SKSE plugin.

## Run

From this directory:

```powershell
uv sync
uv run server
```

Per-message Socket.IO diagnostics are emitted at the normal service log level:
received `player:update` payloads and relayed `player:state` / `player:exit` /
`player:disconnect` payloads. The game console logs its transmitted
`player:update` and received `player:state` / `player:exit` /
`player:disconnect` messages. At the default 20 Hz update rate this
is intentionally verbose.

The server listens on port `3000` on all interfaces. The SKSE plugin defaults
to `http://127.0.0.1:3000` for local testing.

- `GET /health` returns a basic health check.
- `GET /poc` returns a human-readable proof-of-concept response.
- `player:enter` and `player:exit` carry no payload and mark whether the player
  is placed in the game world. A client connects out of the world (main menu),
  enters once its character is in a loaded cell, and exits when a loading screen
  or the main menu opens. Both are acknowledged with `{"ok": true}` and are
  idempotent. The server forgets the player's checkpoint and action stream on
  exit, but keeps its appearance and equipment.
- `player:exit` is relayed to peers with `playerId`, telling them to remove the
  player's actor while keeping its appearance and equipment. `player:enter` is
  not relayed: the next `player:state` places the player again.
- `player:update` carries the local player's position, heading, measured speed,
  explicit movement-animation state, jump phase, bow draw and per-hand spell
  casting (`bowDrawn`, `castingLeft`, `castingRight`, optional and defaulting to
  `false`; see `specs/011-bow-and-spell-presentation-sync.md`), blocking
  (`blocking`, optional and defaulting to `false`; see
  `specs/012-off-hand-block-and-bash-sync.md`), whether the player is dying or
  dead (`dead`, optional and defaulting to `false`; see
  `specs/015-player-death-sync.md`), whether the player rides a mount
  (`mounted`, optional and defaulting to `false`; the position fields then
  describe the mount; see `specs/016-mount-riding-sync.md`), whether that
  player or its mount swims (`swimming`, optional and defaulting to `false`;
  see `specs/017-remote-actor-world-isolation.md`), cell and worldspace form
  IDs,
  occupied furniture reference FormID, graph epoch, and monotonically increasing
  checkpoint sequence number. It is rejected with `not in world` outside
  `player:enter` and `player:exit`.
- `player:state` relays a validated transform to every other connected player.
- `animation:action` carries a validated, ordered semantic action. The
  vocabulary is `attack/normal`, `attack/left`, `attack/dual`, and `attack/bash`.
  Every attack carries a boolean `power`. A power attack or power bash also
  carries a `direction` (`standing`, `forward`, `backward`, `left`, or `right`),
  which a plain attack must not. See `specs/013-power-attack-sync.md`.
- `player:action` relays that action to every other connected player after its
  graph epoch, drawn-weapon, and unoccupied-furniture checkpoint prerequisites
  are verified.
- `mount:transition` carries the start of an animated mount (`enter`) or
  dismount (`exit`), with the `graphEpoch` and `checkpointSequence` of the
  `player:update` sent right before it. It is accepted only against a mounted,
  living latest checkpoint of that epoch, and each transition must be anchored
  to a newer checkpoint than the previous one. `player:mount` relays it to every
  other connected player. See `specs/016-mount-riding-sync.md`.
- `player:appearance` carries the player's latest character name and appearance
  (race, sex, weight, head parts, hair color, face morphs, and tints) with an
  increasing revision. It is relayed to other players with `playerId`. The
  server stores the latest appearance per connection and sends every stored
  appearance to a newly connected client. See
  `specs/006-player-appearance-sync.md`.
- `player:equipment` carries the player's latest equipped items (worn armor,
  right- and left-hand weapon or torch, and equipped `ammo`) as load-order
  independent `{plugin, formId}` references with an increasing revision. A hand
  holding a spell is `{"kind": "spell"}` without a reference. It uses the same
  relay, storage, and late-join lifecycle as `player:appearance`. See
  `specs/007-player-equipment-sync.md`.
- `player:disconnect` tells peers to remove the disconnected player's actor
  and forget all of its state. The server pings every client every 5 seconds
  and disconnects one that does not answer within 10 more, so a crashed or
  unreachable player is removed within 10 seconds.
- `latency:probe` is acknowledged immediately with `{"ok": true, "serverTimeMs": ...}`
  and never relayed. The plugin sends one per second, shows the acknowledgement
  round trip as its ping, and uses `serverTimeMs` to estimate the server clock.
- `player:update` may carry an optional `sentAtMs`: the source's emit time on the
  server clock. It is relayed on `player:state` but never stored or recorded, and
  a missing or invalid value is dropped without rejecting the checkpoint. Peers
  use it to show the remote packet delay.

Attacks are intentionally not another field in `player:update`: a snapshot
field would collapse repeated attacks. The plugin emits one separate
`animation:action` message for every observed melee attack or bash and receives it as
`player:action` on peers.

The plugin publishes at 20 Hz. Peers in different exterior cells of the same
worldspace remain synchronized; interiors require an exact cell match. The
server retains each connection's latest sequence number so stale or reordered
updates are rejected.

## Admin panel

The server hosts a web admin panel at `http://127.0.0.1:3000/admin/`, with a
**Dashboard** (connected players, active captures and replays, stored
recordings, uptime), **Replay management** (capture, replay, delete), and
**Load testing** (many bots replaying one recording; see below). It is a
React and TypeScript app in `admin-ui/`; see `admin-ui/README.md` to build it or
run it in development. Until it is built, `/admin/` explains how, and the REST
API below works regardless.

The panel talks to the server in two ways. Commands are REST calls under `/api`
(documented at `/docs`). Live state is a full snapshot pushed over the `/admin`
Socket.IO namespace on connect and after every change, at most twice a second.
The namespace is separate from the player namespace, so an open panel is never
counted as a player. There is no authentication: like the rest of this POC, do
not expose the port to the public Internet.

## Recordings

Recording is controlled from the panel (or `POST /api/recordings`), per player,
and runs until stopped. Nothing is recorded by default. Starting a capture writes
the player's current appearance, equipment, and latest checkpoint first (preceded
by `player:exit` when the player is out of the world), so a recording that begins
mid-session replays on its own. After that every accepted `player:enter`,
`player:exit`, `player:update`, `player:appearance`, `player:equipment`,
`animation:action`, and `mount:transition` of that player is appended. A capture
also ends when the player disconnects or the server shuts down, and a failing
disk ends only the capture, never the relay.

A recording is one append-only JSON Lines file, `recordings/<id>.jsonl`, with the
ID `YYYYMMDD-HHMMSS-xxxxxx`. It has a `header` line (label, player, start time),
one `message` line per event (`t` in milliseconds from the start, the event name,
and the validated payload exactly as relayed), and an `end` line with the reason.
A file without an `end` line is an interrupted capture and still replays; a torn
last line is ignored. Recordings are ignored by Git.

| Endpoint | |
| --- | --- |
| `GET /api/state` | Everything the panel shows: players, recordings, playbacks, batches |
| `GET /api/recordings/{id}` | One recording with its timeline markers |
| `POST /api/recordings` `{playerId, label?}` | Start capturing a connected player |
| `POST /api/recordings/{id}/stop` | Stop a capture |
| `DELETE /api/recordings/{id}` | Delete a recording that is not being captured |
| `POST /api/playbacks` `{recordingId, autoplay?, rate?, loop?, startMs?}` | Replay a recording |
| `POST /api/playbacks/{id}/play`, `/pause` | Resume or pause |
| `POST /api/playbacks/{id}/seek` `{positionMs}` | Jump to a time |
| `PATCH /api/playbacks/{id}` `{rate?, loop?}` | Speed (0.1 to 4) and looping |
| `DELETE /api/playbacks/{id}` | Stop a replay and remove its virtual player |

## Replaying a recording

A replay runs inside the server as a **virtual player** (`replay:<id>`). Every
connected client sees it as another peer: it is relayed as `player:state`,
`player:action`, `player:mount`, `player:appearance`, `player:equipment`,
`player:exit`, and `player:disconnect`, and clients that join mid-replay receive
its appearance and equipment. While the recorded player was out of the world,
the virtual player exits too and sends no checkpoints. Nothing in the
plugin distinguishes it from a real player.

Clients accept a peer's state only with a rising `sequence`, a non-decreasing
`graphEpoch`, contiguous `actionSequence` values, and rising appearance and
equipment revisions. A replay that pauses, seeks backwards, or loops cannot reuse
a recording's own numbers, so the virtual player numbers everything itself and
only takes the recording's content (`server/playback/virtual_player.py`):

- **Seek** does not replay what it skips. It sends the appearance, equipment, and
  checkpoint in effect at the target, and starts a new graph epoch so clients drop
  attack actions queued for the old position. Appearance and equipment are resent
  only if they differ from what clients already show.
- **Pause** freezes the timeline and holds the actor in place: its last checkpoint
  is repeated four times a second with locomotion stopped (idle, speed zero, not
  jumping), so it stands still instead of running in place and late joiners still
  see it. Playing from the end starts over.
- **Speed** scales the timeline; the recorded animation speed is not adjusted, so
  motion at other than 1x looks faster or slower than the animation implies.

FastAPI's interactive HTTP documentation is available at
`http://127.0.0.1:3000/docs`.

## Load testing

A **batch** replays one recording with many bots at once, for load testing and
comparing builds. Bot `i` of `N` starts at `i × duration / N`, moved by up to
`jitter / 2` of that spacing either way, so the bots do different things at any
moment. Every bot loops. A **seed** fixes the layout: the same seed and settings
give the same start positions and speeds, so runs can be repeated across
builds. Optional settings:

- **Ramp-up**: bots join one by one over this time instead of all at once. A bot
  that joins later starts further along, so the spacing is kept.
- **Speed variation**: each bot plays at `1 ± rateSpread`, so the spacing drifts
  instead of repeating exactly every loop.
- **Connection**: `virtual` bots (`bot:<batch>-<n>`) are replayed inside the
  server like a panel replay. `socket` bots are loopback Socket.IO clients that
  go through the player ingestion path (validation, relay, per-connection
  delivery). They run in the server's own process and event loop, so their
  client-side work counts against the server's CPU.

Bots are named after the recorded player with a number (`Lydia #3`). They are
left out of the snapshot's `players` list and appear only under their batch.
Batches exist only while the server runs.

| Endpoint | |
| --- | --- |
| `POST /api/batches` `{recordingId, players, transport?, seed?, jitter?, rateSpread?, rampUpSeconds?}` | Start a batch (1 to 500 bots) |
| `POST /api/batches/{id}/pause`, `/resume` | Pause or resume every bot |
| `DELETE /api/batches/{id}` | Stop a batch and remove its bots |

## Replay from the command line

`replay` is a Socket.IO test client that connects like a player and sends a
recording through the real ingestion path (validation, relay, and capture
included). Unlike a panel replay, it is a real connection with its own session
ID. Start the server, then in another terminal run:

```powershell
uv run replay recordings/20260921-101500-a1b2c3.jsonl
```

It reads the same `.jsonl` files the server writes, and numbers its messages
itself, so `--loop` keeps working past the first pass. Useful options include
`--url http://192.168.1.20:3000`, `--speed 2` to replay twice as fast,
`--no-timing` to send as fast as acknowledgements allow, and `--loop` for
continuous playback. The client waits for the server acknowledgement by default
and stops if the server rejects a message.

## Connecting from another computer

Create this file under the remote player's Skyrim installation:

`Data/SKSE/Plugins/skyrim-mp-server.txt`

Put the shared server's reachable URL on its first line, for example:

```text
http://192.168.1.20:3000
```

Allow inbound TCP port `3000` through the server machine's firewall. This POC
has no authentication or encryption, so do not expose it directly to the public
Internet.
