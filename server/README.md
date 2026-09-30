# Skyrim Multiplayer server

Go server for the SKSE plugin: binary WebSocket with protobuf messages for
players, a REST API and event feed for the admin panel. The wire protocol and
its validation are in `../specs/network.md`; recordings, replays, and load
testing in `../specs/recording-and-replay.md`.

## Run

From this directory, with Go 1.24 or newer:

```sh
go run ./cmd/server                       # listens on 0.0.0.0:3000
go run ./cmd/server --listen 127.0.0.1:3100 --config server.toml --verbose
```

`--verbose` logs every rejected message and virtual-player detail at debug
level. The plugin defaults to `http://127.0.0.1:3000` and connects to `/ws`.

- `GET /ws` is the player WebSocket.
- `GET /health` returns `{"status":"ok"}`.
- `/admin/` serves the built admin panel; `/api` is its API.

## Configuration

Every key is optional. Link-dependent limits live here, so a homelab and a
remote server differ only in this file.

```toml
listen = "0.0.0.0:3000"
recordings_directory = "recordings"
admin_ui_directory = "admin-ui/dist"
# Batches per second to each client. 40 keeps batching under 25 ms.
send_rate_hz = 40

[limits]
messages_per_second = 120   # per connection; exceeding it closes the connection
message_burst = 240
actions_per_second = 10     # per player; extra attacks are rejected
action_burst = 10
ordered_backlog = 4096      # unsent ordered messages before a slow client is dropped
hello_timeout_seconds = 5

[liveness]
ping_interval_seconds = 5   # WebSocket pings, answered by the client's transport thread
timeout_seconds = 10
```

Unknown keys are an error, so a typo does not silently keep a default.

## Tests and tools

```sh
go test ./...                             # unit and black-box WebSocket tests
go test -race ./...
go run ./cmd/loadgen --bots 250 --server-pid <pid>
go run ./cmd/convert-recordings recordings
```

- `cmd/loadgen` connects bots as real WebSocket clients, walking in a square
  (`--spread`, `--speed`) or replaying a recording (`--recording`). Each bot
  sends its send time in microseconds as the checkpoint sequence, so receivers
  on the same host measure latency across processes. It reports entries and
  bytes received per second, latency percentiles, and with `--server-pid` the
  server's CPU.
- `cmd/convert-recordings` converts version 2 recordings, from the Python
  server, to version 3 in place and keeps each original as `<id>.v2`.
- `../proto/generate.sh` regenerates `internal/wire/pb` and the plugin's
  nanopb code from `../proto/`.

## Layout

| Package | Responsibility |
| --- | --- |
| `cmd/server` | Entry point and wiring |
| `internal/config` | TOML configuration and defaults |
| `internal/session` | WebSocket accept, hello, reader, writer, liveness, limits |
| `internal/validate` | Field validation of client messages |
| `internal/hub` | The one goroutine that owns world state, subscriptions, and delivery order |
| `internal/world` | Player state and ordering rules, name indexes |
| `internal/delivery` | Per-recipient outboxes and batch writing |
| `internal/wire` | Hand-written encoding of hot messages, quantization, generated `pb` |
| `internal/recording` | Format version 3, store, captures, v2 converter, timeline markers |
| `internal/playback` | Virtual players and replay sessions |
| `internal/loadtest` | In-server bot batches |
| `internal/admin` | REST API, event feed, panel files |

## Admin panel

The panel (`admin-ui/`, see its README) shows connected players, captures,
replays, and load-test batches. Commands are REST calls under `/api`; live
state is a server-sent event stream at `/api/events` that pushes a full
snapshot on connect and after changes, at most twice a second.

| Endpoint | |
| --- | --- |
| `GET /api/state` | Everything the panel shows |
| `GET /api/events` | Event stream of the same snapshot |
| `GET /api/recordings/{id}` | One recording with its timeline markers |
| `POST /api/recordings` `{playerId, label?}` | Start capturing a connected player |
| `POST /api/recordings/{id}/stop` | Stop a capture |
| `DELETE /api/recordings/{id}` | Delete a recording that is not being captured |
| `POST /api/playbacks` `{recordingId, autoplay?, rate?, loop?, startMs?}` | Replay a recording |
| `POST /api/playbacks/{id}/play`, `/pause` | Resume or pause |
| `POST /api/playbacks/{id}/seek` `{positionMs}` | Jump to a time |
| `PATCH /api/playbacks/{id}` `{rate?, loop?}` | Speed (0.1 to 4) and looping |
| `DELETE /api/playbacks/{id}` | Stop a replay and remove its virtual player |
| `POST /api/batches` `{recordingId, players, seed?, jitter?, rateSpread?, rampUpSeconds?}` | Start a batch of in-server bots (1 to 500) |
| `POST /api/batches/{id}/pause`, `/resume` | Pause or resume every bot |
| `DELETE /api/batches/{id}` | Stop a batch and remove its bots |

Errors answer `{"detail": "..."}`. Player IDs are slots as decimal strings.

## Connecting from another computer

On the remote player's machine, set the server's reachable URL in
`skyrim-mp.ini` next to `skyrim-mp.dll`:

```ini
[Network]
ServerEndpoint = http://192.168.1.20:3000
```

Allow inbound TCP port 3000 through the server machine's firewall. There is
no authentication, so do not expose the port to the public Internet. For a
remote server, terminate TLS in a reverse proxy and use an `https://`
endpoint; the plugin connects with `wss://`.
