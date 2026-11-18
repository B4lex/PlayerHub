# Recording and Replay

## Scope

The server hosts an admin panel that captures a connected player's behavior,
replays recordings as virtual players every client sees, runs load-test batches
of replayed bots, manages sessions, roles, kicks, and bans, shows Global
chat live, and charts the server's history ([Metrics](metrics.md)). Replays and bots enter the hub through the same methods as
connections ([Interest and delivery](interest-and-delivery.md)), so nothing in
the plugin distinguishes them from real players. `cmd/loadgen` runs bots as
real WebSocket clients out of process.

## Components

| Module | Responsibility |
| --- | --- |
| `internal/recording` | Format version 7, `Store`, `Captures`, `Markers` |
| `internal/playback` | `VirtualPlayer` numbering, `Session` timeline, `Manager`, `HubPlayer` |
| `internal/loadtest` | `PlanBots`, in-server `Batch`es and their `Manager` |
| `internal/admin` | REST routes under `/api`, the `/api/events` feed, panel files, the request guard |
| `internal/adminauth` | Password hash, logins, login routes, the gate in front of the API |
| `internal/adminclient` | The admin API for `cmd/loadgen` and `cmd/tradebot`: login, issuing sessions, reading players |
| `cmd/loadgen` | Networked bots, latency and throughput measurement |
| `admin-ui/` | React and TypeScript panel: login, Dashboard, Statistics, Sessions, Roles, Replay management, Load testing |

## Recordings

One append-only JSON Lines file per capture, `server/recordings/<id>.jsonl`:

```text
{"type":"header","format":"playerhub-recording","version":7,"id":...,"label":...,"playerId":"7","playerName":...,"startedAt":...}
{"type":"message","t":0,"message":{"enter":{}}}
{"type":"end","t":75266,"reason":"stopped"}
```

A message line holds a validated `ClientMessage` in protojson, `t` in
milliseconds from the start. An appearance is recorded as stored, with the
session's display name as `name` ([Network](network.md#sessions)). A missing
`end` is an interrupted capture; a torn last line is dropped. Every message is
validated on load as a connection's would be. IDs become file names and URL
segments, so they must match `[A-Za-z0-9][A-Za-z0-9_-]{0,63}` before any path
is built. Other versions are rejected: a capture is cheap to redo, and
converters would have to follow every wire change.

A capture is started per player from the panel and runs until stopped or the
player disconnects. It starts with the player's presence (`Enter` or `Exit`),
appearance, equipment, effects, context, and checkpoint at time zero, so it
replays without the earlier session, then records every accepted `Enter`,
`Exit`, `Context`, `Checkpoint`, `Action`, `Appearance`, `Equipment`, and
`Effects`; never chat. The hub hands encoded lines to the capture's writer
goroutine without waiting for the disk; a capture whose queue (4096 lines)
fills is ended rather than stalling the hub, and a disk error ends only the
capture.

## Virtual player

A replay runs in the server, as a hub player of kind `KindReplay` without an
outbox, because pause and seek need the server to own the clock.

Receivers accept checkpoints only with rising sequences, contexts only with
non-decreasing graph epochs, actions only when contiguous, and presentation
only with rising revisions. A recording's own numbers break these after a seek
back or a loop, so `VirtualPlayer` numbers every message itself:

| Field | Rule |
| --- | --- |
| checkpoint `sequence` | its own counter |
| checkpoint `time` | when it is due on the timeline (the emit time for a seek, hold, or snapshot), so receivers see the recorded spacing even when the timer fires late |
| context `graph_epoch` | the recording's epoch plus an offset; after a seek or loop the next context starts at `last + 1` |
| action `action_sequence` | its own counter, so skipped actions leave no gap |
| action `checkpoint_sequence`, `graph_epoch` | the latest emitted checkpoint's and context's |
| appearance, equipment, effects `revision` | its own counters; resent only when the content differs from what clients show |

Timeline:

- **Seek** does not replay what it skips, since state is latest-wins and actions
  are edges. It emits the appearance, equipment, effects (an empty list when the
  replay showed some before the first recorded ones), context with a new epoch,
  and checkpoint in effect at the target, or `Exit` when the player was out of
  the world there. The new epoch makes clients drop attacks queued for the old
  position; the context's held idle is re-entered as a late joiner's
  ([Idle presentation](idle-presentation.md#re-entry)).
- A recorded `Enter` places nothing by itself: the next context or checkpoint
  sends `Enter`, preceded by the context in effect when none came since.
- **Pause** freezes the timeline and resends the last checkpoint with
  locomotion stopped: idle (sneaking idle if sneaking), speed 0, not jumping,
  keeping stance, hand state, and position. Late joiners get it in `PeerEnter`.
- **Play from the end** starts over; **Loop** seeks to zero at the end.
  **Speed** (0.1 to 4) scales the timeline only.

All emission happens in one goroutine per replay; control calls change the
timeline under a lock and wake it, so a seek takes effect between two messages.
Timing uses the monotonic clock.

## Load testing

A batch puts many players in the world from one recording, for load testing and
comparing performance across builds. Several batches can run at once.

- A batch is `N` replay sessions of one recording, each looping from its own
  offset, as hub players of kind `KindBot`.
- Bot `i` starts at `i × D / N` of the duration `D`, plus a uniform offset up to
  `jitter / 2` of the spacing either way, so neighbours never swap places. With
  `rateSpread`, each bot plays at a uniform rate in `1 ± rateSpread`. Offsets
  and rates come from two PCG generators seeded from the seed, so changing one
  setting keeps the other's layout; the server picks and reports a seed when
  none is given.
- With a ramp-up of `R` seconds, bot `i` joins at `R × i / N`, that much further
  along its timeline (scaled by its rate), so the spacing matches a batch
  started at once.
- Recorded positions are replayed unchanged, so bots never stand in terrain.
- Bots are named after the recorded player with their number (`Lydia #3`),
  shortened to the 128-byte name limit.
- A bot that cannot join is reported failed; the rest keep running.

`cmd/loadgen` runs networked bots: real WebSocket clients that walk randomly in
a square (`--spread` 20000 dense, 100000 sparse) or replay a recording laid out
like a batch.

- Display names are `W` (walker) or `R` (replay), six hexadecimal digits, `_`,
  and the bot's number (`W43ae13_7`).
- Before the ramp-up, loadgen asks the admin API (`--admin`) for every bot's
  session ([Network](network.md#sessions)), passing the token it kept, and saves
  digits and tokens whenever one changes (`--sessions`, default
  `<user cache>/playerhub/loadgen-<host>.json`, mode 0600), so a server keeps
  one session per bot across runs and reissues them after a store reset.
  Sessions issued before a failure are saved too, or their names would stay
  taken without a token. A display name another session holds stops the run
  before any bot connects. `--admin ""` keeps no file and takes new sessions
  through the per-address allowance like a player.
- Every bot connects from one address, so more bots than
  `limits.max_players_per_address` need that cap off
  ([Admission](network.md#admission)).
- Each bot stamps its checkpoint sequence and `time` with the send time in
  microseconds, strictly rising, so a receiver on the same host measures latency
  without a wire field; `--server-pid` adds the server's CPU from `/proc`.

## Admin API

The panel at `/admin/`, the API, and `--pprof` are served only on
`admin_listen` (default `127.0.0.1:3001`; empty serves none), never on the
player address `listen`, so a reverse proxy forwarding the game port cannot
expose them. `--admin-listen` overrides the setting and must differ from
`listen`; `--pprof` needs it. The routes are listed in the
[server README](../server/README.md#admin-panel). Errors, including the
guard's and gate's, answer `{"detail": ...}`; player IDs are slots as decimal
strings.

`admin.Guard` runs before the [login](#admin-login) check and stops web pages
the operator visits from driving the address through the browser:

- **Host:** every request must name the address by an IP literal, `localhost`,
  or a host name in `admin_hosts` (without port, case-insensitive; default
  empty), else 403. This defeats DNS rebinding; IP literals stay allowed because
  a page served elsewhere never shares their origin.
- **Other sites:** a `Sec-Fetch-Site` of `cross-site` or `same-site` gets 403
  for any method, since browsers set it on navigations and no-cors loads too,
  which covers costly GETs such as `/debug/pprof/profile`. A page on another
  port of the same host is same-site. Without the header,
  `http.CrossOriginProtection` refuses a non-GET/HEAD/OPTIONS request whose
  `Origin` host differs from `Host`. Requests with neither header pass.
- **JSON bodies:** a route that reads a body answers 415 unless `Content-Type`
  is `application/json`. Browsers send only text, form, and multipart bodies
  cross-site without a preflight, and the API answers no preflight.

### Admin login

Goal: the panel can face another machine without trusting everyone who reaches
`admin_listen`. Non-goals: accounts, roles, or an audit trail (one shared
password); logins that outlive a restart.

- **Password:** `admin_password_hash` holds
  `pbkdf2-sha256$<iterations>$<salt>$<key>` (unpadded standard base64; salt at
  least 16 bytes, key 32, iterations 100000..10000000); anything else is a
  configuration error. `cmd/hashpassword` makes one with 600000 iterations
  (OWASP's PBKDF2-HMAC-SHA256 figure, about 70 ms per check) and a fresh 16-byte
  salt from a password of at least 12 characters, read twice without echo from
  a terminal or as one line of standard input. It runs from the source tree and
  is not shipped: the server holds no secret of its own (no pepper, unsigned
  random tokens), so a hash made anywhere works on any server, and
  `golang.org/x/term` stays out of the server binary. Empty (default) leaves the
  address open with a startup warning.
- **Gate:** `adminauth.Gate` wraps the admin mux inside the guard and serves
  `GET /api/login` (`{required, signedIn}`), `POST /api/login` `{password}`,
  and `POST /api/logout`. With a password, every other path except `/admin` and
  `/admin/...` (the panel's files, which hold no data) answers 401 without a
  live login, `/api/events` and `/debug/pprof/` included. Without one,
  everything passes and a login answers 204 without a cookie, so tools need not
  know which kind of server they talk to.
- **Login:** a right password answers 204 with the cookie `playerhub_admin`: a
  random 32-byte base64url token, `HttpOnly`, `SameSite=Strict`, `Path=/`,
  `Max-Age` 7 days, and `Secure` when the request came over TLS or with
  `X-Forwarded-Proto: https`, so plain HTTP on loopback or an SSH tunnel works.
  A wrong one answers 401. Logs: `admin logged in` (info) and `admin login
  refused` (warning) with the client address.
- **Attempt limits**, checked before hashing: per client address
  (`admission.ClientAddress`; IPv6 per /64) a bucket of 5 refilled at 5 a
  minute, then over all addresses 20 refilled at 2 a second, which bounds the
  hash's CPU. At most 4096 addresses are tracked; while that many hold spent
  buckets, new addresses are refused. A refusal answers 429 with `Retry-After`.
- **Logins** are kept in memory, keyed by the token's SHA-256 so lookup time
  does not depend on a partial match. Each lasts 7 days from login, not
  extended by use; at most 64 are held, evicting the one ending soonest. Logout
  ends it and clears the cookie. Each login has a context that cancels every
  request it authorized, so an open `/api/events` stream closes on logout or
  expiry.
- **Tools:** `adminclient.NewClient` logs in once with
  `PLAYERHUB_ADMIN_PASSWORD` (an environment variable, so it stays out of the
  process list) and keeps the cookie; a 401 names the variable.
- **Panel:** `LoginGate` (`admin-ui/src/state/Login.tsx`) asks `GET /api/login`
  before mounting the live state and shows `LoginPage` while a login is needed.
  Any 401 other than the login form's returns to it. EventSource hides a
  refused stream's status, so a closed feed makes the gate ask again; if the
  server cannot be asked, the panel shows and the feed's refusal asks later.
  The sidebar has a log-out control while a password is set. The form carries a
  hidden fixed user name so password managers file the password.

### Events

Live state is one full snapshot pushed as `event: state` on `/api/events` on
connect and after each change, at most one per 0.5 s: the first change after a
quiet period is sent at once, later ones collapse into one trailing send.
Changes cost nothing while no panel listens.

- A replay's position is not pushed while playing; the panel extrapolates from
  the last position and rate, and a per-playback `revision` decides between a
  command's response and a push that crossed it.
- `players` lists connected players by connection time, then slot, then
  virtual players in playback start order, so rows keep their place. Each shows
  `visible` and the interest controls and rates of
  [Interest and delivery](interest-and-delivery.md#admin-controls); a connected
  player also its `address` (after `trusted_proxies`), `sessionId`, and
  `roleId` or null. Bots are listed per batch with status, offset, rate, and
  position.
- `roles` lists every [role](network.md#roles) as `{id, name, color, holders}`,
  `color` as `#rrggbb`, `holders` the live sessions holding it.
- Global chat lines are pushed as `event: chat`, one `{atMs, playerId, name,
  text}` per accepted line (`admin.ChatLine`): server wall time, the speaker's
  slot and display name, the trimmed text. The hub hands each line to
  `hub.Options.OnGlobalChat` on its loop; `Feed.Chat` returns at once without
  listeners, else queues it per panel, at most 256, dropping new lines for a
  panel that far behind rather than holding the hub. Nearby and Whisper lines
  are never sent, and the server keeps no chat, so a panel gets only lines
  relayed while connected.

### Panel pages

- **Dashboard** (`features/dashboard/DashboardPage`): the chat panel
  (`ChatPanel`) above the player table shows the chat events since the page
  loaded, the last 200, from a separate context (`useChat`) so a busy chat
  re-renders only the panel. It has a fixed height, follows new lines while
  scrolled to the bottom, and has no input. The player table shows name and
  slot, location, movement state, received peers and traffic, and status
  badges, with fixed column widths and single-line cells so live values never
  resize it. A holder's name follows its role's letter mark
  (`components/RoleBadge`'s `RoleLetter`). A connected player's row command
  menu (`components/Menu`) hides or shows, sets the role, kicks, or bans
  ([Kicks and bans](network.md#kicks-and-bans)), each confirmed in a modal
  (`components/ConfirmDialog`); the ban dialog's checkbox also bans the address
  and is disabled when the snapshot has none. Directed exclusions have no
  control and are set through the API.
- **Statistics** (`features/statistics/StatisticsPage`): the server's history
  over a range, fetched from `GET /api/statistics`
  ([Statistics page](metrics.md#statistics-page)).
- **Sessions** (`features/sessions/SessionsPage`): the store's sessions
  ([Sessions](network.md#sessions)), 50 per page. Not in the snapshot, which
  would grow with every session kept: the page fetches
  `GET /api/sessions?search=&role=&offset=&limit=` when its search (debounced
  0.25 s), role filter, or page changes, after its own commands, and when the
  snapshot's connected players, bans, or role holders change. The route lists
  live and banned sessions, most recently used first, then by ID; `search`,
  trimmed, matches part of a display name without case (`instr`, so `_` is
  literal) or a whole session ID; `role` keeps that role's holders; `offset`
  0 or more, `limit` 1..200 (default 50), else 422. It answers
  `{sessions, total}`; a session is `{id, displayName, createdAtMs,
  lastUsedAtMs, expiresAtMs, banned, playerId, roleId}`, `expiresAtMs` null
  when banned, `playerId` and `roleId` null when absent. The role filter lives
  in the page address (`?role=`), which the Roles page links to; `?search=`
  fills the search when the page opens, which the Statistics page links to. A row's menu
  renames (disabled while connected), sets the role, lifts bans (only when
  banned), and deletes. Set role (`features/roles/SetRoleDialog`, shared with
  the Dashboard) applies to a connected player at once. The rename dialog
  checks the display name rule before enabling confirm and keeps a refusal such
  as a held name inside the dialog, since the modal covers the toasts. Lift
  deletes every snapshot ban with the session's `sessionId`.
- **Roles** (`features/roles/RolesPage`): each role as the game shows it
  (`components/RoleBadge`, colors from `lib/roles`, which derives the letter
  and text color as the game does), its color, and its holder count, linking to
  Sessions filtered by the role. Create is disabled at the server's limit; a
  row's menu changes or deletes. One dialog serves both with name, color, and
  the resulting look, and keeps refusals inside; delete says how many players
  lose the role.

Route answers beyond the README:

- `POST /api/players/{id}/kick` 204; `POST /api/players/{id}/ban` `{address}`
  requires the boolean and answers 201 with the ban, 404 for a player not
  connected, 422 for an address that cannot be banned. A ban's `address` is the
  IPv4 address or IPv6 /64, or null. `DELETE /api/bans/{id}` 204 or 404.
- `PUT /api/sessions/{id}/role` `{roleId}` (ID or null) 200 with the body, 422
  for a missing field or unknown role, 404 for no live session.
  `PATCH /api/sessions/{id}` `{displayName}` 200, 422 for a name failing
  `validate.DisplayName`, 404 for no live session, 409 for a held name or a
  connected session. `DELETE /api/sessions/{id}` 204 or 404.
- `POST /api/roles` `{name, color}` 201 with the role, 422 for a name failing
  `validate.RoleName` or a color not `#rrggbb`, 409 for a held name or at the
  limit; `PATCH /api/roles/{id}` takes both fields and answers 200 or 404;
  `DELETE /api/roles/{id}` 204 or 404.

### Command feedback

The panel shows a command's outcome without waiting for the server, since a
snapshot produced before the command can arrive after its response and the next
can be 0.5 s away.

- A command on something the snapshot lists registers a `Prediction`
  (`admin-ui/src/lib/predictions`) through `useAction`: hide or show, kick, ban,
  and set a player's role; change and delete a role; stop and delete a
  recording; a replay's play, pause, seek, rate, loop, and close; a batch's
  pause, resume, and close. `useServerState()` applies held predictions to the
  snapshot in order, so pages show the outcome from the click.
- A failed command drops its prediction and shows the refusal as a toast. A
  succeeded one is held until the snapshot shows the outcome or can no longer
  show it (`Prediction.confirmed`), at most `CONFIRM_TIMEOUT_MS` (5 s). A
  replay's prediction lasts only while its command is in flight, since the
  response carries the new state, adopted by `revision`.
- A prediction replaces earlier ones with its `key` (one property of one
  object). Players match by slot and `connectedAtMs`, since slots are reused.
  `useAction` sends one control's commands one after another, so the server
  applies them in order. A predicted replay or bot starts or stops moving at the
  click, the server's at arrival, so its position steps by the round trip times
  the rate when the server's state takes over.
- A command that creates something (a capture, a replay, a batch, a role) has
  no outcome to predict, since the server assigns the ID. Its button
  (`components/Button`, `busy`) refuses clicks at once and shows a spinner
  (`components/Spinner`) only after `--busy-delay` (150 ms). The response's
  object is listed at once, a replay through `adoptPlayback`, which holds it
  for the same 5 s; a response for a replay closed meanwhile is ignored.
- The Sessions page holds its own row changes (`HeldChange`): delete hides the
  row, lift clears the banned state, rename shows the returned name. A change is
  dropped when its command fails, otherwise when a page requested after the
  answer arrives. While the rows answer an earlier query, the table is marked
  busy and a spinner shows after the same delay.

## Known limitations

- Without `admin_password_hash` the admin API has no authentication: it deletes
  recordings, spawns virtual players, kicks and bans, renames and deletes
  sessions, and issues sessions past the per-address allowance, so
  `admin_listen` must not face the Internet.
- The server does not end TLS on `admin_listen`: from another machine, the
  password and cookie cross in the clear unless a proxy or tunnel encrypts them.
- One password for everyone; nothing records which admin acted; logins end on
  restart. Someone failing from many addresses can keep the operator from
  logging in until the buckets refill.
- The [request guard](#admin-api) stops only web pages. Browsers without fetch
  metadata (Safari before 16.4, Firefox before 90) can still be made to send
  cross-site GETs, which get 401 with a password set, since `SameSite=Strict`
  keeps the cookie off them. A link to the panel from another site is refused,
  which rules out single sign-on that redirects back.
- Replaying at a speed other than 1x advances position at the scaled rate but
  keeps the recorded `speed` and animation state.
- A replay reaches every connected client; recordings cannot be edited or
  renamed, and players are not captured automatically.
- Batches do not survive a server restart.
- Recordings are tied to the protocol version of their server.
