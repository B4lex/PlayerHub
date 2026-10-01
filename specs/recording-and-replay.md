# Recording and Replay

## Status

Implemented in Go and verified on Linux:

- `go test -race ./...` in `server/` covers the format, converter, store,
  captures, markers (`internal/recording`), virtual-player numbering and replay
  sessions with a fake clock (`internal/playback`), bot layout
  (`internal/loadtest`), and, over a real WebSocket, capturing a live player,
  replaying it to another client, and the admin API's errors
  (`internal/admin`).
- `cmd/convert-recordings` converted a version 2 fixture and left a converted
  file alone; `cmd/loadgen` replayed it with 20 bots and the server rejected
  none of their messages.
- `npm run typecheck`, `npm test`, and `npm run build` in `server/admin-ui`,
  and the built panel loaded in headless Chromium against the Go server, live
  over the event feed, with no console errors.

Not verified: how a replayed player looks in game in every case (see
[Engine assumptions](#engine-assumptions)), and real recordings made by the
Python server converted to version 3.

## Scope

The server hosts an admin panel that captures a connected player's behavior,
replays recordings as virtual players every client sees, and runs load-test
batches of many replayed bots. Replays and bots enter the hub through the same
methods as connections ([Interest and delivery](interest-and-delivery.md)), so
nothing in the plugin distinguishes them from real players. `cmd/loadgen` runs
bots as real WebSocket clients out of process.

## Components

| Module | Responsibility |
| --- | --- |
| `internal/recording` | Format version 3, `Store`, `Captures`, `ConvertV2`, `Markers` |
| `internal/playback` | `VirtualPlayer` numbering, `Session` timeline, `Manager`, `HubPlayer` |
| `internal/loadtest` | `PlanBots`, in-server `Batch`es and their `Manager` |
| `internal/admin` | REST routes under `/api`, the `/api/events` feed, panel files |
| `cmd/loadgen` | Networked bots, latency and throughput measurement |
| `cmd/convert-recordings` | Version 2 to 3 conversion in place |
| `server/admin-ui/` | React and TypeScript panel: Dashboard, Replay management, Load testing |

## Recordings

One append-only JSON Lines file per capture, `server/recordings/<id>.jsonl`:

```text
{"type":"header","format":"skyrim-mp-recording","version":3,"id":...,"label":...,"playerId":"7","playerName":...,"startedAt":...}
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
`[A-Za-z0-9][A-Za-z0-9_-]{0,63}` and checked before any path is built. Version 2
files, whose lines hold the Socket.IO JSON payloads, are not read;
`cmd/convert-recordings` converts them in place, splitting each checkpoint into
a `Context` (written when its space, epoch, or furniture changes, and after
every enter) and a quantized `Checkpoint`, and keeps the original as `<id>.v2`.

A capture is started per player from the panel and runs until stopped or the
player disconnects. It starts with the player's presence (`Enter` or `Exit`),
appearance, equipment, context, and checkpoint at time zero, so it replays
without the earlier session. It records every accepted `Enter`, `Exit`,
`Context`, `Checkpoint`, `Action`, `Appearance`, and `Equipment`; chat is never
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
| appearance, equipment `revision` | its own counters; resent only when the content differs from what clients show |

Timeline:

- **Seek** does not replay what it skips, since state is latest-wins and actions
  are edges. It emits the appearance, equipment, context (with a new epoch),
  and checkpoint in effect at the target, or `Exit` when the recorded player
  was out of the world there. The new epoch makes the plugin drop attacks still
  queued for the old position.
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
  batch. Each takes a new session ([Network](network.md#sessions)) whose display
  name is `Walker`, or the recorded player's name, then six hexadecimal digits
  picked per run and the bot's number (`Walker 43ae13 #7`), so a later run
  against the same server does not find its display names taken; the sessions
  stay in the server's database until they expire. Each stamps its checkpoint
  sequence with the send time in microseconds, which is strictly rising, so a
  receiver on the same host measures latency without a wire field, and its
  `time` with the send time; `--server-pid` adds the server's CPU from `/proc`.

## Admin API

Commands are REST under `/api` ([server README](../server/README.md#admin-panel));
errors answer `{"detail": ...}`, and player IDs are slots as decimal strings.
Live state is one full snapshot pushed as a server-sent event (`event: state`)
at `/api/events` on connect and after each change, coalesced to at most one per
0.5 s: the first change after a quiet period is sent at once, and later ones
collapse into one trailing send. Changes cost nothing while no panel listens. A
replay's position is not pushed while playing; the panel extrapolates from the
last reported position and rate, and a per-playback `revision` decides between
a command's response and a push that crossed it. Bots are not listed in
`players`; each batch lists its bots with status, offset, rate, and position.
Each player shows `visible`, the number of peers it receives, and the
interest controls and rates described in
[Interest and delivery](interest-and-delivery.md#admin-controls).

## Engine assumptions

Not checked in game:

1. A remote actor whose last checkpoint is idle at a fixed position stands
   still with its stance kept.
2. A virtual player that seeks (position jump, new graph epoch) is treated like
   a teleporting peer, in the same or another cell.
3. Replaying at a speed other than 1x looks acceptable: position advances at the
   scaled rate, but `speed` and the animation state are the recorded ones.

## Known limitations

- The admin API has no authentication: it deletes recordings and spawns
  virtual players, so the port must not face the Internet.
- A replay reaches every connected client; recordings cannot be edited or
  renamed, and players are not captured automatically.
- Batches do not survive a server restart.
- Recordings are tied to the protocol version of their server.
