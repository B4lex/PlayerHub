# Skyrim Multiplayer server

Local FastAPI and Socket.IO synchronization server for the SKSE plugin.

## Run

From this directory:

```powershell
uv sync
uv run server
```

Per-message Socket.IO diagnostics are emitted at the normal service log level:
received `player:update` payloads and relayed `player:state` / `player:left`
payloads. The game console logs its transmitted `player:update` and received
`player:state` / `player:left` messages. At the default 20 Hz update rate this
is intentionally verbose.

The server listens on port `3000` on all interfaces. The SKSE plugin defaults
to `http://127.0.0.1:3000` for local testing.

- `GET /health` returns a basic health check.
- `GET /poc` returns a human-readable proof-of-concept response.
- `player:update` carries the local player's position, heading, measured speed,
  explicit movement-animation state, jump phase, bow draw and per-hand spell
  casting (`bowDrawn`, `castingLeft`, `castingRight`, optional and defaulting to
  `false`; see `specs/011-bow-and-spell-presentation-sync.md`), blocking
  (`blocking`, optional and defaulting to `false`; see
  `specs/012-off-hand-block-and-bash-sync.md`), cell and worldspace form IDs,
  occupied furniture reference FormID, graph epoch, and monotonically increasing
  checkpoint sequence number.
- `player:state` relays a validated transform to every other connected player.
- `animation:action` carries a validated, ordered semantic action. The
  vocabulary is `attack/normal`, `attack/left`, `attack/dual`, and `attack/bash`.
  Every attack carries a boolean `power`. A power attack or power bash also
  carries a `direction` (`standing`, `forward`, `backward`, `left`, or `right`),
  which a plain attack must not. See `specs/013-power-attack-sync.md`.
- `player:action` relays that action to every other connected player after its
  graph epoch, drawn-weapon, and unoccupied-furniture checkpoint prerequisites
  are verified.
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
- `player:left` tells peers to remove the disconnected player's actor.
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

Recording is disabled by default. To enable it for a server launch, set the
`SKYRIM_MP_RECORDING_ENABLED` boolean flag before starting the server:

```powershell
$env:SKYRIM_MP_RECORDING_ENABLED = "true"
uv run server
```

When enabled, every accepted `player:update`, `player:appearance`,
`player:equipment`, and `animation:action` is persisted
to one ordered `recordings/<playerId>.json` timeline. Each file is atomically
refreshed after a message, so it remains valid and replayable if the server is
interrupted. The `playerId` is the current Socket.IO session ID (there is no
stable account ID in this POC), and its filename is URL encoded when needed.
Generated recordings are ignored by Git; the bundled example remains tracked.

FastAPI's interactive HTTP documentation is available at
`http://127.0.0.1:3000/docs`.

## Replay a recorded player

`replay` is a Socket.IO test client that connects using the WebSocket transport
and emits recorded state checkpoints and animation actions. Start the server,
then in another terminal run:

```powershell
uv run replay recordings/player_movement.example.json
```

The recording is either an array of legacy update payloads or an object
containing a `messages` array. New timeline entries contain `event` with
`player:update`, `player:appearance`, `player:equipment`, or
`animation:action`, plus the
corresponding `payload`. Put
timing beside a payload with `delayMs` (delay before that message), or
`timestampMs` (an absolute time within the recording). A top-level
`defaultDelayMs` applies to entries without timing.

Legacy transform-only recordings remain usable and are treated as
`player:update`. The replay client derives a missing `speed` from adjacent
positions, defaults a missing `jumping` flag to grounded, defaults a missing
`weaponDrawn` flag to sheathed, defaults a missing `furnitureRefId` to zero, and
defaults a missing `graphEpoch` to the initial epoch 1. Transform-only recordings
made before action persistence cannot reconstruct attacks that are not present
in the file.

Useful options include `--url http://192.168.1.20:3000`, `--speed 2` to replay
twice as fast, `--no-timing` to send as fast as acknowledgements allow, and
`--loop` for continuous playback. The client waits for the server acknowledgement
by default and stops if the server rejects a message (for example, a stale
sequence number).

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
