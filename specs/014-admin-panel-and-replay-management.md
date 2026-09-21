# Admin Panel and Replay Management

## Status

Implemented and verified on Linux: the server side by unit tests and by an
end-to-end run against a live server with real Socket.IO clients that enforce
the plugin's receive rules (see [Verification](#verification)), the panel by
type checks, unit tests, and headless-browser screenshots. Not verified: the
Windows plugin build and how a replayed virtual player looks in game (see
[Engine assumptions](#engine-assumptions)).

## Goal

Give the server an admin panel, and use it for replay management:

- **Capture** a chosen connected player's behavior until told to stop.
- **Replay** a recording as a virtual player that every client sees, with pause,
  seek to any time, speed, and loop, like a media player.
- Leave room for more pages. The panel starts with **Dashboard** and **Replay
  management**.

This supersedes the environment-flag recording of specs 006 and 007
("When recording is enabled, appends ..."): recording is now per player and
started from the panel. Protocol and server versions ship together, so nothing
here is compatible with the old recording files or CLI options.

## Non-goals

- Authentication. The server is an unauthenticated POC (see the server README);
  the admin API can delete recordings and spawn virtual players, so the port
  must not face the Internet.
- Replaying to a chosen subset of clients. A replay reaches everyone connected.
- Editing recordings, renaming them, or capturing every player automatically.
- Changing the plugin or the sync protocol between clients and server. The
  replay is expressed entirely in the existing `player:*` events.

## Design

### Recordings

One append-only JSON Lines file per capture (`server/recordings/<id>.jsonl`):
a `header`, timed `message` lines carrying validated payloads as relayed, and an
`end` line. The previous format rewrote the whole file on every message, which
is quadratic for a capture that runs until stopped. Appending is also crash
safe: a missing `end` is an interrupted capture, and a torn last line is
dropped. Recording IDs become file names and URL segments, so they are
restricted to `[A-Za-z0-9][A-Za-z0-9_-]{0,63}` and checked before any path is
built.

A capture starts with the player's current appearance, equipment, and latest
checkpoint at time zero, so it replays without the earlier session. A capture
failure ends the capture, never the message being relayed.

### Replay as a virtual player

A replay runs in the server, not as a client, because pause and seek need the
server to own the clock. Its actor is a synthetic peer `replay:<id>` whose
events are relayed to every client, with appearance and equipment also stored
in the registry so late joiners receive them.

The plugin accepts a peer's `player:state` only if `sequence` rises and
`graphEpoch` does not fall, an action only if `actionSequence` is exactly the
previous plus one (and its epoch matches the latest checkpoint), and appearance
or equipment only if `revision` rises (`SocketIOClient.cpp`, per player ID).
A recording's own numbers break these rules after a seek back or a loop. The
`VirtualPlayer` therefore numbers every message itself:

| Field | Rule |
| --- | --- |
| `sequence` | Its own counter, one per emitted checkpoint |
| `graphEpoch` | The recording's epoch plus an offset. A seek or loop advances the offset so the next checkpoint starts epoch `last + 1`; never decreases |
| `actionSequence` | Its own counter, one per emitted action, so skipped actions leave no gap |
| `checkpointSequence`, action `graphEpoch` | The latest emitted checkpoint's, which the plugin requires an action to match |
| appearance, equipment `revision` | Its own counters; resent only when the content differs from what clients already show |

Timeline behavior:

- **Seek** does not replay what it skips (state is latest-wins, actions are
  edges). It emits the appearance, equipment, and checkpoint in effect at the
  target and starts a new graph epoch, which makes the plugin drop attack
  actions still queued for the old position.
- **Pause** freezes the timeline and holds the pose: the last checkpoint is
  repeated every 0.25 s with `movementState` idle (sneaking idle if sneaking),
  `speed` 0, and `jumping` false, with stance, hand state, and position kept.
  Repeating a checkpoint unchanged would leave the remote actor running in
  place; not repeating it would leave clients that join during the pause
  without an actor, since the server keeps no checkpoints for late joiners.
- **Play from the end** starts over. **Loop** does the same at the end, as a
  seek to zero. **Speed** (0.1 to 4) scales the timeline only.

All emission happens in one task per replay. Control calls only change the
timeline state and wake it, so ordering is trivial and a seek can take effect
between two emitted messages. Timing uses the monotonic clock and durations in
seconds, never frame counts; peers at other frame rates only see 20 Hz
recorded checkpoints.

### Admin API

Commands are REST under `/api` (routes are coroutines, because playbacks and
captures are event-loop state). Live state is one full snapshot pushed over the
`/admin` Socket.IO namespace on connect and after each change, coalesced to at
most one per 0.5 s: the first change after a quiet period is sent at once, and
later ones collapse into one trailing send. The namespace is separate from `/`,
so a panel is never a player. A replay's position is not pushed while playing;
the panel extrapolates from the last reported position and rate, and a
per-playback `revision` decides between a command's response and a push that
crossed it on the wire.

## Engine assumptions

Not checked in game:

1. A remote actor whose checkpoints are repeated with `movementState` idle at a
   fixed position stands still with its stance kept.
2. A virtual player that seeks (position jump, new graph epoch) is treated like
   a teleporting peer, in the same or another cell.
3. Replaying at a speed other than 1x looks acceptable: position advances at the
   scaled rate, but `speed` and the animation state are the recorded ones.

## Verification

- `uv run python -m unittest discover -s tests` from `server/`: format, store,
  capture, virtual player, session, admin API, broadcaster, and the handler
  hooks.
- End-to-end against a live server: two players, a panel connection, a capture
  of a live player, a replay of a real 75 s capture with pause, backward and
  forward seeks, loop, and a late joiner. Every relayed message was checked
  against the plugin's receive rules, and none was stale.
- `npm run typecheck`, `npm test`, and `npm run build` in `server/admin-ui`, and
  screenshots of both pages in light and dark themes and at phone width.
