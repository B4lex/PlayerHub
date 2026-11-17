# Recording and Replay

## Scope

The server hosts an admin panel that captures a connected player's behavior,
replays recordings as virtual players every client sees, and runs load-test
batches of many replayed bots. Replays and bots enter the hub through the same
methods as connections ([Interest and delivery](interest-and-delivery.md)), so
nothing in the plugin distinguishes them from real players. `cmd/loadgen` runs
bots as real WebSocket clients out of process. The Dashboard also shows the
Global chat channel live, relayed by the hub ([Chat](network.md#chat)).

## Components

| Module | Responsibility |
| --- | --- |
| `internal/recording` | Format version 7, `Store`, `Captures`, `Markers` |
| `internal/playback` | `VirtualPlayer` numbering, `Session` timeline, `Manager`, `HubPlayer` |
| `internal/loadtest` | `PlanBots`, in-server `Batch`es and their `Manager` |
| `internal/admin` | REST routes under `/api`, the `/api/events` feed, panel files, the request guard |
| `internal/adminauth` | Admin password hash, logins, the login routes, and the gate in front of the API |
| `internal/adminclient` | The admin API for `cmd/loadgen` and `cmd/tradebot`: login, issuing sessions, reading players |
| `cmd/loadgen` | Networked bots, latency and throughput measurement |
| `admin-ui/` | React and TypeScript panel: login page, Dashboard, Sessions, Replay management, Load testing |

## Recordings

One append-only JSON Lines file per capture, `server/recordings/<id>.jsonl`:

```text
{"type":"header","format":"playerhub-recording","version":7,"id":...,"label":...,"playerId":"7","playerName":...,"startedAt":...}
{"type":"message","t":0,"message":{"enter":{}}}
{"type":"end","t":75266,"reason":"stopped"}
```

A message line holds a validated `ClientMessage` in protojson, `t` in
milliseconds from the start. An appearance is recorded as the server stored it,
with the player's session display name as its `name`
([Network](network.md#sessions)), so a replay shows the name the player had. A
missing `end` is an interrupted capture, and a torn last line is dropped. Every
message is validated on load as the server validates a connection's. IDs become
file names and URL segments, so they are restricted to
`[A-Za-z0-9][A-Za-z0-9_-]{0,63}` and checked before any path is built. Other
versions are rejected on load; recordings are not kept compatible across
format changes, since a capture is cheap to redo and converters would have to
follow every wire change.

A capture is started per player from the panel and runs until stopped or the
player disconnects. It starts with the player's presence (`Enter` or `Exit`),
appearance, equipment, effects, context, and checkpoint at time zero, so it
replays without the earlier session. It records every accepted `Enter`, `Exit`,
`Context`, `Checkpoint`, `Action`, `Appearance`, `Equipment`, and `Effects`;
chat is never
recorded. The hub encodes a line and hands it to the capture's writer
goroutine without waiting for the disk; a capture whose queue (4096 lines)
fills is ended rather than stalling the hub, and a disk error ends only the
capture.

## Virtual player

A replay runs in the server, not as a client, because pause and seek need the
server to own the clock. It is a hub player of kind `KindReplay` without an
outbox.

Receivers accept a peer's checkpoint only with a rising sequence, its context
only with a non-decreasing graph epoch, its actions only when contiguous, and
presentation only with a rising revision. A recording's own numbers break
these rules after a seek back or a loop, so `VirtualPlayer` numbers every
message itself and never modifies the recorded one:

| Field | Rule |
| --- | --- |
| checkpoint `sequence` | its own counter |
| checkpoint `time` | when it is due on the replay timeline (the emit time for a seek, hold, or snapshot), so receivers see the recorded spacing even when the timer fires late |
| context `graph_epoch` | the recording's epoch plus an offset; after a seek or loop the next context starts at `last + 1`; never decreases |
| action `action_sequence` | its own counter, so skipped actions leave no gap |
| action `checkpoint_sequence`, `graph_epoch` | the latest emitted checkpoint's and context's |
| appearance, equipment, effects `revision` | its own counters; resent only when the content differs from what clients show |

Timeline:

- **Seek** does not replay what it skips, since state is latest-wins and actions
  are edges. It emits the appearance, equipment, effects, context (with a new
  epoch), and checkpoint in effect at the target (no effects before the first
  recorded ones, sent as an empty list when the replay showed some), or `Exit` when the recorded player
  was out of the world there. The new epoch makes the plugin drop attacks still
  queued for the old position. The context carries the idle held at the target,
  which receivers enter as a late joiner does
  ([Idle presentation](idle-presentation.md#re-entry)).
- A recorded `Enter` places nothing by itself: the next context or checkpoint
  sends `Enter`, and a checkpoint without a context since then sends the
  context in effect first.
- **Pause** freezes the timeline and sends the last checkpoint once more with
  locomotion stopped: movement state idle (sneaking idle if sneaking), speed 0,
  not jumping, keeping stance, hand state, and position. Late joiners get that
  stored checkpoint in `PeerEnter`, so nothing is repeated while paused.
- **Play from the end** starts over. **Loop** does the same at the end, as a seek
  to zero. **Speed** (0.1 to 4) scales the timeline only.

All emission happens in one goroutine per replay; control calls only change
the timeline under a lock and wake it, so a seek takes effect between two
messages. Messages go to the hub loop in order. Timing uses the monotonic clock
and durations in milliseconds of timeline.

## Load testing

A batch puts many players in the world from one recording, for load testing and
for comparing performance across development stages. Several batches can run at
once, including from one recording.

- A batch is `N` ordinary replay sessions of one recording, each looping from
  its own offset, as hub players of kind `KindBot`.
- Bot `i` starts at `i × D / N` of the recording's duration `D`, plus a uniform
  offset up to `jitter / 2` of the spacing either way, so neighbours never swap
  places. With `rateSpread`, each bot plays at a uniform rate in
  `1 ± rateSpread`. Offsets and rates come from two PCG generators seeded from
  the seed, so changing one setting keeps the other's layout; the server picks
  and reports a seed when none is given.
- With a ramp-up of `R` seconds, bot `i` joins at `R × i / N` and starts that much
  further along its timeline (scaled by its rate), so the spacing matches a batch
  started at once.
- Recorded positions are replayed unchanged, so bots are never placed inside
  terrain or walls.
- Bots are named after the recorded player with their number (`Lydia #3`),
  shortened to fit the 128-byte name limit.
- A bot that cannot join is reported failed and leaves the rest of its batch
  running.
- `transport` is always `virtual`. Bots that exercise the network run in
  `cmd/loadgen`: real WebSocket clients that walk randomly in a square
  (`--spread` 20000 dense, 100000 sparse) or replay a recording laid out like a
  batch. Each bot's display name is `W` for a walker or `R` for a replay, then
  six hexadecimal digits, `_`, and the bot's number (`W43ae13_7`). Before the
  ramp-up, loadgen asks the admin API (`--admin`) for every bot's session
  ([Network](network.md#sessions)), passing the token it kept, and saves the
  digits and tokens to a file (`--sessions`, default
  `<user cache>/playerhub/loadgen-<host>.json`, mode 0600) whenever one
  changes. A later run resumes the same sessions, so a server keeps one
  session per bot however often loadgen runs, and a server whose store was
  reset issues them again. Sessions issued before a failure are saved too, or
  their names would stay taken without a token. A display name another
  session holds stops the run before any bot connects. `--admin ""` keeps no
  file and takes new sessions under new digits, through the per-address
  allowance like a player. Issued or not, every bot connects from one address,
  so more bots than `limits.max_players_per_address` need that cap off
  ([Admission](network.md#admission)). Each stamps its checkpoint
  sequence with the send time in microseconds, which is strictly rising, so a
  receiver on the same host measures latency without a wire field, and its
  `time` with the send time; `--server-pid` adds the server's CPU from `/proc`.

## Admin API

The panel at `/admin/`, the API, and `--pprof` profiling are served only on
`admin_listen` (default `127.0.0.1:3001`; empty serves none of them), never on
the player address `listen`, so a reverse proxy that forwards the game port
cannot expose them. `--admin-listen` overrides the setting, which must differ
from `listen`, and `--pprof` needs it.

`admin.Guard` stops web pages the operator visits from driving the address
through the browser, and runs before the [login](#admin-login) check:

- **Host:** every request, panel files and pprof included, must name the
  address by an IP literal, `localhost`, or a name in `admin_hosts` (host
  names without a port, compared without case; default empty); others get
  403. This defeats DNS rebinding, where a page's own domain is made to resolve
  to the admin address so the browser treats the API as same-origin. IP
  literals stay allowed because a page served from elsewhere can never share
  their origin.
- **Other sites:** any request whose `Sec-Fetch-Site` is `cross-site` or
  `same-site` gets 403, whatever its method: browsers set the header on
  navigations and no-cors loads too, so this also covers GETs with a cost,
  such as `/debug/pprof/profile` and a recording load. A page on another
  port of the same host counts as same-site. Without the header,
  `http.CrossOriginProtection` answers 403 to a non-GET/HEAD/OPTIONS request
  whose `Origin` host differs from `Host`. Requests with neither header, such
  as `adminclient`'s and typed URLs in browsers without fetch metadata, pass.
- **JSON bodies:** a route that reads a body answers 415 unless
  `Content-Type` is `application/json` (parameters allowed). Browsers send
  only text, form, and multipart bodies cross-site without a preflight, and
  the API answers no preflight.

### Admin login

Goal: the panel can face another machine, such as a dedicated server's
operator, without trusting everyone who reaches `admin_listen`. Non-goals:
accounts, roles, or a record of who did what (one shared password); logins
that outlive a restart.

- **Password:** `admin_password_hash` holds
  `pbkdf2-sha256$<iterations>$<salt>$<key>` (unpadded standard base64; salt at
  least 16 bytes, key 32, iterations 100000..10000000); anything else is a
  configuration error. `--hash-password` makes one with 600000 iterations
  (OWASP's PBKDF2-HMAC-SHA256 figure; about 70 ms per check) and a fresh
  16-byte salt, from a password of at least 12 characters read twice without
  echo from a terminal, or as one line of standard input otherwise, and exits.
  PBKDF2 is in Go's standard library, so the hash needs no dependency; the
  hidden prompt uses `golang.org/x/term`. Empty (the default) leaves the
  address open as before, with a startup warning, so a local or development
  server needs no setup.
- **Gate:** `adminauth.Gate` wraps the admin mux inside the guard. It serves
  `GET /api/login` (`{required, signedIn}`), `POST /api/login` `{password}`,
  and `POST /api/logout`; with a password, every other path except `/admin`
  and `/admin/...` (the panel's files, which hold no data) answers 401 without
  a live login, `/api/events` and `/debug/pprof/` included. Without a password
  everything passes, and a login answers 204 without a cookie, so tools need
  not know which kind of server they talk to.
- **Login:** a right password answers 204 with the cookie `playerhub_admin`: a
  random 32-byte token, base64url, `HttpOnly`, `SameSite=Strict`, `Path=/`,
  `Max-Age` of 7 days, and `Secure` when the request came over TLS or with
  `X-Forwarded-Proto: https`, so the cookie still works over plain HTTP on
  loopback and through an SSH tunnel. A wrong one answers 401, a body that is
  not JSON 415. The log has `admin logged in` (info) and `admin login refused`
  (warning) with the client address.
- **Attempt limits:** checked before the hash runs. Per client address
  (`admission.ClientAddress`, so behind `trusted_proxies` the forwarded one;
  IPv6 per /64), a bucket of 5 refilled at 5 a minute; then over all
  addresses, 20 refilled at 2 a second, which bounds the hash's CPU under many
  addresses. At most 4096 addresses are tracked; when that many hold spent
  buckets, attempts from new addresses are refused. A refusal answers 429
  with `Retry-After`.
- **Logins:** kept in memory, keyed by the token's SHA-256 so a lookup's time
  does not depend on how much of a guess is right. Each lasts 7 days from
  logging in, not extended by use; at most 64 are held, and the one that would
  end soonest makes room. Logout ends it and clears the cookie. Each login has
  a context that ends with it, and every request it authorized is cancelled
  then, so an open `/api/events` stream closes on logout or expiry.
- **Tools:** `adminclient.NewClient` logs in once with the password in
  `PLAYERHUB_ADMIN_PASSWORD`, an environment variable so it stays out of the
  process list, and keeps the cookie in a jar; a 401 from the API names the
  variable.
- **Panel:** `LoginGate` (`admin-ui/src/state/Login.tsx`) asks
  `GET /api/login` before mounting the live state, and shows `LoginPage` while
  a password is required and the panel is not logged in. Any 401 from a
  command, other than the login form's own, returns to the login page.
  EventSource gives up on a refused stream without its status, so a feed that
  closes makes the gate ask again. If the server cannot be asked, the panel is
  shown and the feed's refusal asks later. The sidebar footer has a log-out
  control while a password is set. The form carries a hidden fixed user name
  so password managers file the password.

### Requests and events

Commands are REST under `/api` ([server README](../server/README.md#admin-panel));
errors, the guard's and the gate's refusals included, answer `{"detail": ...}`, and player
IDs are slots as decimal strings.
Live state is one full snapshot pushed as a server-sent event (`event: state`)
at `/api/events` on connect and after each change, coalesced to at most one per
0.5 s: the first change after a quiet period is sent at once, and later ones
collapse into one trailing send. Changes cost nothing while no panel listens. A
replay's position is not pushed while playing; the panel extrapolates from the
last reported position and rate, and a per-playback `revision` decides between
a command's response and a push that crossed it. Bots are not listed in
`players`; each batch lists its bots with status, offset, rate, and position.
Global chat lines are pushed on the same stream as `event: chat`, one
`{atMs, playerId, name, text}` per accepted line (`admin.ChatLine`): the
server's wall time, the speaker's slot and display name, and the trimmed
text. The hub hands each line to `hub.Options.OnGlobalChat` on its loop, and
`Feed.Chat` returns at once while no panel listens; otherwise it queues the
line for each panel, at most 256 waiting, dropping new lines for a panel that
far behind rather than holding the hub. Nearby and Whisper lines are never
sent, and the server keeps no chat, so a panel gets only lines relayed while
it is connected.
`players` lists connected players by connection time, then slot, followed by
virtual players in the order their playbacks started, so rows keep their place
across pushes. Each player shows `visible`, the number of peers it receives, and the
interest controls and rates described in
[Interest and delivery](interest-and-delivery.md#admin-controls). A connected
player also shows its `address`, after `trusted_proxies`, its `sessionId`,
and its `roleId` or null. The snapshot's `roles` lists every
[role](network.md#roles) as `{id, name, color, holders}`, `color` as
`#rrggbb` and `holders` the live sessions holding it.

Above the player table, the Dashboard's chat panel
(`features/dashboard/ChatPanel`) shows the chat events received since the
page loaded, kept by `ServerStateProvider` in a separate context (`useChat`)
so a busy chat re-renders only the panel, the last 200 lines. Each line
shows the local time of `atMs`, the speaker's name, and the text, which
wraps. The log has a fixed height so the table below does not move, scrolls,
and follows new lines while scrolled to within 8 px of the bottom; scrolled
up, it keeps its place. It has no input and no clear control.

The Dashboard's player table (`features/dashboard/DashboardPage`) shows each
player's name and slot, location, movement state, received peers and
traffic, and status badges. Its address, position, and `replacedPerSecond`
stay in the snapshot but are not shown. The table has fixed column widths
(`table-layout: fixed`) and single-line cells cut with an ellipsis, so live
values never resize rows or columns. A connected player's row starts with a
command menu (`components/Menu`, a popover) that hides or shows the player,
sets their role, kicks, or bans them
([Kicks and bans](network.md#kicks-and-bans)). A holder's name follows its
role's letter mark (`components/RoleBadge`'s `RoleLetter`). Every
command first asks in a modal dialog (`components/ConfirmDialog`); the ban
dialog has a checkbox that also bans the player's address, disabled when the
snapshot has none. Bans are listed and lifted on the Sessions page. Directed exclusions
have no panel control and are set through the API
([Admin controls](interest-and-delivery.md#admin-controls)).
`POST /api/players/{id}/kick` answers 204;
`POST /api/players/{id}/ban` `{address}` requires the boolean and answers 201
with the ban, 404 for a player that is not connected, and 422 for an address
that cannot be banned; `DELETE /api/bans/{id}` answers 204 or 404. A ban's
`address` is the banned IPv4 address or IPv6 /64, or null.

The Sessions page (`features/sessions/SessionsPage`) lists the sessions in
the store ([Sessions](network.md#sessions)), 50 per page with a pager, and
renames or deletes one. It is not in the snapshot, which would grow with every
session the server keeps: the page fetches
`GET /api/sessions?search=&role=&offset=&limit=` again when its search
(debounced 0.25 s), role filter, or page changes, after its own commands, and
when the snapshot's connected players, bans, or roles' holders change. The route lists live sessions and banned
ones, most recently used first, then by ID; `search`, trimmed, matches part of
a display name without case (`instr`, so `_` is literal) or a whole session
ID; `role`, a role's ID, leaves that role's holders; `offset` is 0 or more
and `limit` 1..200 (default 50), else 422. It
answers `{sessions, total}`, `total` counting every match; a session is
`{id, displayName, createdAtMs, lastUsedAtMs, expiresAtMs, banned, playerId, roleId}`,
`expiresAtMs` null when banned, `playerId` the slot of its connection or
null, and `roleId` null without a role. A row shows the name and ID, the
role's badge, last and first use, expiry (none while
connected or banned), and connected and banned badges. The page keeps the
role it filters by in its address (`?role=`), which the Roles page links to,
and offers it as a select beside the search while there are roles. A row
starts with a
command menu like the Dashboard's (`components/Menu`) holding rename, set
role, lift ban
(on a banned session only), and delete, each confirmed in a
`components/ConfirmDialog`. Set role opens `features/roles/SetRoleDialog`,
shared with the Dashboard: a select of no role and every role, with the
chosen one's look beside it; it applies to a connected player at once.
`PUT /api/sessions/{id}/role` `{roleId}`, a role's ID or null, answers 200
with the same body, 422 for a body without the field or a role that does not
exist, and 404 for no live session. Rename is disabled while connected; its dialog
holds the name field, checked against the display name rule before the
confirm button enables, and keeps a refusal such as a held name inside the
dialog, since the modal covers the toasts. Lift deletes every ban in the
snapshot's `bans` whose `sessionId` is the session's, since each ban command
adds one, and is disabled while the snapshot has none for it. `PATCH /api/sessions/{id}` `{displayName}` answers 200, 422 for a name
that fails `validate.DisplayName`, 404 for no live session, and 409 for a held
name or a connected session. `DELETE /api/sessions/{id}` answers 204 or 404.

The Roles page (`features/roles/RolesPage`) lists the snapshot's roles, each
as the game shows it (the letter mark and the whole badge,
`components/RoleBadge`, as the game's chip in `lib/roles`'s
`roleChipColors` over a fixed dark backdrop in either theme), with its color and the number of holders, which
links to the Sessions page filtered by the role. Its button creates a role
and is disabled at the server's limit; a row's command menu changes or
deletes it. Both use one dialog with a name field, a color input, and the
look that results, which keeps a refusal such as a held name inside it. The
delete confirmation says how many players lose the role.
`POST /api/roles` `{name, color}` answers 201 with the role, 422 for a name
that fails `validate.RoleName` or a `color` that is not `#rrggbb`, and 409
for a held name or a server already at its limit;
`PATCH /api/roles/{id}` takes the same body, both fields required, and
answers 200 with them, or 404; `DELETE /api/roles/{id}` answers 204 or 404.
The panel takes the letter and the text color on a role's color as the game
does (`lib/roles`).

### Command feedback

The panel shows a command's outcome without waiting for the server, because a
snapshot produced before the command can arrive after its response and the
next one can be 0.5 s away.

A command on something the snapshot already lists registers a `Prediction`
(`admin-ui/src/lib/predictions`) through `useAction`: hiding or showing,
kicking, banning, and giving a role to a player; changing and deleting a
role; stopping and deleting a recording; a replay's
play, pause, seek, rate, loop, and close; a batch's pause, resume, and close.
`useServerState()` returns the snapshot with the held predictions applied in
the order they were made, so every page shows the outcome from the click, and
a confirmation dialog closes when it is confirmed. A failed command drops its
prediction and reports the refusal as a toast. A succeeded one keeps it until
the snapshot shows the outcome or can no longer show it, such as a hidden
player who left (`Prediction.confirmed`), and at most 5 s
(`CONFIRM_TIMEOUT_MS`), which only a change made elsewhere meanwhile takes. A
replay's prediction lasts only while its command is in flight, since the
response carries the new state and is adopted by `revision`. A prediction
replaces the earlier ones with its `key`, which names one property of one
object, so toggling twice leaves only the last. A player is matched by slot
and `connectedAtMs`, since slots are reused. `useAction` sends the commands of
one control one after another, so the server applies them in the order they
were made. A predicted replay or bot starts or stops moving at the click and
the server's at the request's arrival, so its position steps by the round trip
times the rate when the server's state replaces the prediction.

A command that creates something has no outcome to predict, since the server
assigns the ID: starting a capture, opening a replay, and creating a batch.
Its button (`components/Button`, `busy`) refuses clicks at once and replaces
its icon or label with a spinner (`components/Spinner`) only once the wait
passes `--busy-delay` (150 ms), keeping its width. The object in the response
is listed at once: a recording, a batch, or a role as a succeeded prediction, and a
replay through `adoptPlayback`, which keeps it across snapshots that do not
list it yet for the same 5 s. A response for a replay the panel no longer
holds is ignored, since it closed meanwhile.

The Sessions page is not in the snapshot and holds its own row changes
(`HeldChange`): delete hides the row, lift ban clears the banned state, and
rename, whose dialog waits for the answer, shows the returned name. A change
is dropped at once when its command fails, and otherwise when a page
requested after the command answered arrives. While the rows shown answer an
earlier search or page, the table is marked busy and a spinner shows beside
the search field after the same delay.

## Engine assumptions

Not confirmed in game:

1. A remote actor whose last checkpoint is idle at a fixed position stands
   still with its stance kept.
2. A virtual player that seeks (position jump, new graph epoch) is treated like
   a teleporting peer, in the same or another cell.
3. Replaying at a speed other than 1x looks acceptable: position advances at the
   scaled rate, but `speed` and the animation state are the recorded ones.

## Known limitations

- Without `admin_password_hash` the admin API has no authentication: it
  deletes recordings, spawns virtual players, kicks and bans players, renames
  and deletes sessions, and issues sessions past the per-address allowance, so
  `admin_listen` must not face the Internet, and every program and device that
  reaches it is trusted.
- The server does not end TLS on `admin_listen`: from another machine, the
  password and cookie cross in the clear unless a proxy or tunnel in front
  encrypts them.
- One password for everyone; nothing records which admin acted. Logins end on
  restart. The attempt limits let someone who keeps failing from many
  addresses keep the operator from logging in until the buckets refill.
- The [request guard](#admin-api) stops only web pages, and browsers without
  fetch metadata (Safari before 16.4, Firefox before 90) can still be made to
  send it cross-site GETs, such as a `--pprof` CPU profile the page cannot
  read; with a password set, `SameSite=Strict` keeps the login cookie off
  them, so they get 401. A link to the panel from another site is refused as
  well, which also rules out single sign-on that redirects back to the panel.
- A replay reaches every connected client; recordings cannot be edited or
  renamed, and players are not captured automatically.
- Batches do not survive a server restart.
- Recordings are tied to the protocol version of their server.
