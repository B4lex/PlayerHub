# Skyrim Multiplayer server

Go server for the SKSE plugin: binary WebSocket with protobuf messages for
players, a REST API and event feed for the admin panel. The wire protocol and
its validation are in `../specs/network.md`; recordings, replays, and load
testing in `../specs/recording-and-replay.md`.

## Run

From this directory, with Go 1.24 or newer:

```sh
go run ./cmd/server                       # players on 0.0.0.0:3000, admin on 127.0.0.1:3001
go run ./cmd/server --listen 127.0.0.1:3100 --admin-listen 127.0.0.1:3101 --config server.toml --verbose
```

`--verbose` logs every rejected message and virtual-player detail at debug
level. A plugin built without `xmake f --server_endpoint=...` takes
`http://127.0.0.1:3000` as its official server and connects to `/ws`
([Configuration](../specs/configuration.md#official-and-custom-server)).

On `listen`:

- `GET /ws` is the player WebSocket. Upgrades from web pages are refused: an
  `Origin` header must be absent or name a `ws://` or `wss://` origin, as the
  plugin's does.
- `GET /health` returns `{"status":"ok"}`.

On `admin_listen` only:

- `/admin/` serves the built admin panel; `/api` is its API.
- `/debug/pprof/` with `--pprof`.

## Configuration

Every key is optional. Link-dependent limits live here, so a homelab and a
remote server differ only in this file.

```toml
listen = "0.0.0.0:3000"
admin_listen = "127.0.0.1:3001"   # admin panel, API, and pprof; "" serves none of them
trusted_proxies = []        # e.g. ["127.0.0.1"] behind a local proxy; X-Forwarded-For then names the player
recordings_directory = "recordings"
admin_ui_directory = "admin-ui/dist"
# Batches per second to each client, at most one tick of batching delay.
# Clients play checkpoints back at their sampled spacing, so 20 is smooth.
send_rate_hz = 20

[limits]
messages_per_second = 120   # per connection; exceeding it closes the connection
message_burst = 240
actions_per_second = 10     # per player; extra attacks are rejected
action_burst = 10
ordered_backlog = 4096      # unsent ordered messages before a slow client is dropped
hello_timeout_seconds = 5
max_players = 100           # connected at once; 0 is no cap; replays and bots do not count
max_pending_hellos = 64     # connections still waiting for their hello; over it, upgrades answer 503
new_sessions_per_address_per_hour = 5  # IPv6 per /64; 0 is no limit; resuming is never limited

[liveness]
ping_interval_seconds = 5   # WebSocket pings, answered by the client's transport thread
timeout_seconds = 10

[interest]
enter_radius = 8192         # game units, horizontal
leave_radius = 9830
min_dwell_seconds = 1       # time in view before a peer can leave for range
update_hz = 5               # periodic reevaluation; hard rules apply at once
max_visible = 64            # peers per client, nearest first; 0 is no cap
cap_hysteresis = 1024       # a newcomer must be this much nearer to displace a peer in view
near_distance = 4096        # peers beyond it get far_rate_hz
far_rate_hz = 10            # 0 sends every checkpoint to every peer
minimum_view_radius = 1024  # the smallest radius a client may ask for

[budget]
enabled = false             # true for an uplink that cannot carry everything
uplink_bytes_per_second = 6250000   # the link; 80% of it is used
per_recipient_bytes_per_second = 0  # caps one client's share; 0 is an equal share only
enters_per_second = 20      # new peers per client and second, nearest first

[chat]
nearby_radius = 8192        # game units, horizontal; an interior is heard whole

[trade]
request_radius = 400        # game units, horizontal; both players for an invitation
keep_radius = 1000          # an invited or open trade closes beyond it
invite_seconds = 30         # an unanswered invitation expires

[sessions]
database = "sessions.db"    # SQLite file of player sessions; "" keeps them in memory
lifetime_days = 30          # a session unused for this long is gone, with its display name
```

Players are identified by sessions (`../specs/network.md`, Sessions). The
session database is created on first start, with its write-ahead log next to
it. Deleting it gives every player a new session and frees every display name.

For a homelab with a 50 Mbit/s upload, see `homelab.toml`: the budget on and
32 visible peers, which keeps the nearest peers' p99 under 60 ms with 250
players in one crowd.

`--pprof` serves `net/http/pprof` under `/debug/pprof/` on the admin address
for profiling under loadgen.

Unknown keys are an error, so a typo does not silently keep a default.

## Tests and tools

```sh
go test ./...                             # unit and black-box WebSocket tests
go test -race ./...
go run ./cmd/loadgen --bots 250 --server-pid <pid>
go run ./cmd/tradebot --kit
go run ./cmd/convert-recordings recordings
```

- `cmd/loadgen` gets its bots' sessions from the admin API (`--admin`,
  `http://127.0.0.1:3001`), which skips the per-address allowance for new
  sessions, and keeps their tokens in a file (`--sessions`, by default
  `loadgen-<host>.json` in the user cache directory), so later runs resume the
  same sessions under the same names. Two runs at once against one server need
  different `--sessions` files. More bots than `max_players` need it raised.
  `--admin ""` takes new sessions as a player does, which needs
  `new_sessions_per_address_per_hour = 0`.
- `cmd/loadgen` connects bots as real WebSocket clients, walking in a square
  (`--spread`, `--speed`, `--idle` for a share standing still, `--view-radius`)
  or replaying a recording (`--recording`). Each bot
  sends its send time in microseconds as the checkpoint sequence, so receivers
  on the same host measure latency across processes. It reports entries and
  bytes received per second, latency percentiles overall and for peers within
  `--near` (4096), and with `--server-pid` the server's CPU.
- `cmd/tradebot` is a scripted trade partner for testing trades from the game
  without a second player: it joins as a real client, follows the only other
  player in the world (`--target` picks one), and is driven from its console
  (`help`). See `../specs/trade.md`, Trade bot.
- `cmd/convert-recordings` converts version 2 recordings, from the Python
  server, to version 3 in place and keeps each original as `<id>.v2`.
- `../proto/generate.sh` regenerates `internal/wire/pb` and the plugin's
  nanopb code from `../proto/`.

## Layout

| Package | Responsibility |
| --- | --- |
| `cmd/server` | Entry point and wiring |
| `cmd/loadgen`, `cmd/tradebot` | Networked bots: load testing, a trade partner |
| `internal/adminclient` | The admin API for tools: issuing sessions, reading players |
| `internal/config` | TOML configuration and defaults |
| `internal/session` | WebSocket accept, hello and session authentication, reader, writer, liveness, limits |
| `internal/auth` | Session store: tokens, unique display names, expiry |
| `internal/admission` | Player cap, pending hello cap, new sessions per address, client address behind proxies |
| `internal/validate` | Field validation of client messages |
| `internal/hub` | The one goroutine that owns world state, subscriptions, and delivery order |
| `internal/interest` | Interest rules, exclusions, spatial grid |
| `internal/world` | Player state and ordering rules, display name lookup |
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
| `POST /api/sessions` `{displayName, token?}` | Resume the session of a base64 `token`, or issue one under `displayName` without the per-address allowance; answers `{sessionId, displayName, token}` |
| `PUT /api/players/{id}/hidden` `{hidden}` | Hide a player from everyone, or show it again |
| `POST /api/exclusions`, `DELETE /api/exclusions` `{viewerId, targetId}` | Add or lift a directed exclusion |
| `POST /api/batches` `{recordingId, players, seed?, jitter?, rateSpread?, rampUpSeconds?}` | Start a batch of in-server bots (1 to 500) |
| `POST /api/batches/{id}/pause`, `/resume` | Pause or resume every bot |
| `DELETE /api/batches/{id}` | Stop a batch and remove its bots |

Errors answer `{"detail": "..."}`. Player IDs are slots as decimal strings.

## Connecting from another computer

On the remote player's machine, switch on `Custom Server` in the settings
window's Connection section and enter the server's reachable URL, or set
it in `PlayerHub.ini` next to `PlayerHub.dll`:

```ini
[Connection]
CustomServer = true
ServerEndpoint = http://192.168.1.20:3000
```

Allow inbound TCP port 3000 through the server machine's firewall. Sessions
identify players. The admin panel and API have no authentication and listen
on `admin_listen`, `127.0.0.1:3001` by default, apart from players; keep that
address off the public Internet and reach it through an SSH tunnel if needed.
For a remote server, terminate TLS in a reverse proxy and use an `https://`
endpoint; the plugin connects with `wss://`. Add the proxy's address to
`trusted_proxies` (`127.0.0.1` for `cloudflared` or nginx on the same host),
or every player shares the proxy's allowance of new sessions per hour.
