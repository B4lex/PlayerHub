# Recording and Replay

## Status

Implemented and verified on Linux:

- `uv run python -m unittest discover -s tests` from `server/` covers the
  format, store, capture, virtual player, session, admin API, broadcaster, load
  test batches, and the handler hooks.
- End-to-end against a live server: two players, a panel connection, a capture
  of a live player, and a replay of a real 75 s capture with pause, backward and
  forward seeks, loop, and a late joiner; then three load-test batches of 20
  bots, in-server and socket, with ramp-up, pause, resume, and stop. A watching
  Socket.IO client checked every relayed message against the plugin's receive
  rules: none was stale and there were no violations.
- `npm run typecheck`, `npm test`, and `npm run build` in `server/admin-ui`, and
  headless screenshots of every page in light and dark themes and at phone
  width.

Not verified: how a replayed player looks in game in every case (see
[Engine assumptions](#engine-assumptions)) and how the plugin performs with
hundreds of bots. Replayed players are used in game to test the plugin's
features.

## Scope

The server hosts an admin panel that captures a connected player's behavior,
replays recordings as virtual players every client sees, and runs load-test
batches of many replayed bots. Replays and bots are expressed entirely in the
existing `player:*` events ([Network](network.md)); nothing in the plugin
distinguishes them from real players.

## Components

| Module | Responsibility |
| --- | --- |
| `server/src/server/recording/` | JSON Lines format, `RecordingStore`, `CaptureManager` |
| `server/src/server/playback/` | `VirtualPlayer`, playback sessions, `PlaybackManager`, publishers |
| `server/src/server/loadtest/` | `BatchManager`: bot layout, in-server and socket bots |
| `server/src/server/admin/` | REST routes under `/api`, the `/admin` namespace, `StateBroadcaster`, state snapshot |
| `server/admin-ui/` | React and TypeScript panel: Dashboard, Replay management, Load testing |

## Recordings

One append-only JSON Lines file per capture, `server/recordings/<id>.jsonl`: a
`header`, timed `message` lines carrying validated payloads as relayed, and an
`end` line. Appending keeps a capture that runs until stopped linear (the older
format rewrote the file per message) and crash safe: a missing `end` is an
interrupted capture, and a torn last line is dropped. IDs become file names and
URL segments, so they are restricted to `[A-Za-z0-9][A-Za-z0-9_-]{0,63}` and
checked before any path is built.

A capture is started per player from the panel and runs until stopped or the
player disconnects. It starts with the player's current presence, appearance,
equipment, and latest checkpoint at time zero, so it replays without the earlier
session. It records `player:enter`, `player:exit`, `player:update`,
`player:appearance`, `player:equipment`, and `animation:action`; chat is never
recorded. A capture failure ends the capture, never the message being relayed.

## Virtual player

A replay runs in the server, not as a client, because pause and seek need the
server to own the clock. Its actor is a synthetic peer `replay:<id>` whose events
go through the same relay as real players, with appearance and equipment also
stored in the registry so late joiners receive them.

The plugin accepts a peer's checkpoint only if `sequence` rises and `graphEpoch`
does not fall, an action only if `actionSequence` is the previous plus one and
its epoch matches the latest checkpoint, and appearance or equipment only if
`revision` rises. A recording's own numbers break these rules after a seek back
or a loop, so `VirtualPlayer` numbers every message itself:

| Field | Rule |
| --- | --- |
| `sequence` | its own counter, one per emitted checkpoint |
| `graphEpoch` | the recording's epoch plus an offset; a seek or loop advances the offset so the next checkpoint starts at `last + 1`; never decreases |
| `actionSequence` | its own counter, one per emitted action, so skipped actions leave no gap |
| action `checkpointSequence`, `graphEpoch` | the latest emitted checkpoint's |
| appearance, equipment `revision` | its own counters; resent only when the content differs from what clients show |

Timeline:

- **Seek** does not replay what it skips, since state is latest-wins and actions
  are edges. It emits the appearance, equipment, and checkpoint in effect at the
  target and starts a new graph epoch, which makes the plugin drop attacks still
  queued for the old position.
- **Pause** freezes the timeline and holds the pose: the last checkpoint is
  repeated every 0.25 s with `movementState` idle (sneaking idle if sneaking),
  `speed` 0, and `jumping` false, keeping stance, hand state, and position.
  Repeating it unchanged would leave the actor running in place; not repeating
  it would leave clients that join during the pause without an actor, since the
  server keeps no checkpoints for late joiners.
- **Play from the end** starts over. **Loop** does the same at the end, as a seek
  to zero. **Speed** (0.1 to 4) scales the timeline only.

All emission happens in one task per replay. Control calls only change the
timeline state and wake it, so ordering is trivial and a seek can take effect
between two messages. Timing uses the monotonic clock and durations in seconds;
peers at other frame rates only see 20 Hz recorded checkpoints.

## Load testing

A batch puts many players in the world from one recording, for load testing and
for comparing performance across development stages. Several batches can run at
once, including from one recording.

- A batch is `N` ordinary playback sessions of one recording, each looping from
  its own offset, so every bot numbers its own sequences, epochs, and revisions.
- Bot `i` starts at `i × D / N` of the recording's duration `D`, plus a uniform
  offset up to `jitter / 2` of the spacing either way, so neighbours never swap
  places. With `rateSpread`, each bot plays at a uniform rate in
  `1 ± rateSpread`, so the spacing drifts instead of repeating each loop. Offsets
  and rates come from two generators derived from the seed, so changing one
  setting keeps the other's layout; the server picks and reports a seed when none
  is given.
- With a ramp-up of `R` seconds, bot `i` joins at `R × i / N` and starts that much
  further along its timeline (scaled by its rate), so the spacing matches a batch
  started at once, and appearance and equipment rebuilds on clients are spread
  out.
- Recorded positions are replayed unchanged, so bots are never placed inside
  terrain or walls; the time shift spreads them along the recorded route.
- **In-server** bots publish through the relay as `bot:<batch>-<n>`, loading the
  server's fan-out and the game clients. **Socket** bots are loopback
  `socketio.AsyncClient` connections to the address the admin request arrived at
  (the local end of its connection, not the `Host` header, which names the
  development proxy). They send `player:*` events like the plugin, including
  `player:enter` before the first checkpoint, so parsing, validation, relay, and
  per-connection delivery are exercised; they share the server's process and
  event loop, so their receive work counts against its CPU.
- A bot that cannot connect or drops is reported failed and leaves the rest of
  its batch running.
- Bots are named after the recorded player with their number (`Lydia #3`), so
  they can be told apart in game and in `latest.log`.

## Admin API

Commands are REST under `/api`; routes are coroutines, because playbacks and
captures are event-loop state. Live state is one full snapshot pushed over the
`/admin` Socket.IO namespace on connect and after each change, coalesced to at
most one per 0.5 s: the first change after a quiet period is sent at once, and
later ones collapse into one trailing send. The namespace is separate from `/`,
so a panel is never a player. A replay's position is not pushed while playing;
the panel extrapolates from the last reported position and rate, and a
per-playback `revision` decides between a command's response and a push that
crossed it. Bots are not listed in `players`, which would bloat every snapshot
and bury real players; each batch lists its bots with status, offset, rate, and
position, extrapolated the same way.

## Engine assumptions

Not checked in game:

1. A remote actor whose checkpoints repeat with `movementState` idle at a fixed
   position stands still with its stance kept.
2. A virtual player that seeks (position jump, new graph epoch) is treated like a
   teleporting peer, in the same or another cell.
3. Replaying at a speed other than 1x looks acceptable: position advances at the
   scaled rate, but `speed` and the animation state are the recorded ones.

## Known limitations

- No authentication: the admin API deletes recordings and spawns virtual
  players, so the port must not face the Internet.
- A replay reaches every connected client; recordings cannot be edited or
  renamed, and players are not captured automatically.
- There are no metrics or benchmark reports, and batches and presets do not
  survive a server restart.
- Recordings are tied to the protocol version of their server; actions recorded
  before `power` existed no longer replay.
