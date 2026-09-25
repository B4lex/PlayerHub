# Load Testing

## Status

Implemented and verified on Linux: server unit tests, and an end-to-end run
against a live server where a watching Socket.IO client checked every bot's
relayed stream against the plugin's receive rules (three batches of 20 bots,
in-server and socket, with ramp-up, pause, resume, and stop: no violations).
The panel was checked by type checks, unit tests, a build, and headless
screenshots. Not verified: how the plugin performs with hundreds of bots in game.

## Goal

Put many players in the world from one recording, for load testing and for
comparing performance across development stages. The admin panel gets a
**Load testing** page to start, pause, resume, and stop batches, and several
batches can run at once, including from the same recording.

## Non-goals

- Metrics or benchmark reports. Measure with external tools and logs for now.
- Spreading bots in space. Recorded positions are replayed unchanged, so bots
  are never placed inside terrain or walls; the time shift spreads them along
  the recorded route instead.
- Keeping batches across server restarts, or saved presets.

## Design

A batch is `N` ordinary playback sessions (spec 014) of one recording, each
looping from its own offset. Reusing the session keeps the numbering rules of
the virtual player: every bot numbers its own sequences, epochs, and revisions.

### Layout

Bot `i` starts at `i × D / N`, plus a uniform offset of up to `jitter / 2` of
the spacing either way, so neighbours never swap places. With `rateSpread`,
each bot plays at a uniform rate in `1 ± rateSpread`, so the spacing drifts
rather than repeating on every loop. Offsets and rates come from two random
generators derived from the seed, so changing one setting keeps the other's
layout. The server picks a random seed when none is given and reports it.

With a ramp-up of `R` seconds, bot `i` joins at `R × i / N` and starts that much
further along the timeline (scaled by its rate), so the spacing matches a batch
that started all at once. Bursts of appearance and equipment rebuilds on the
clients are spread out too.

### Connection

- **In-server** bots publish through the same relay as a panel replay, as
  `bot:<batch>-<n>`. This loads the server's fan-out and the game clients.
- **Socket** bots are loopback `socketio.AsyncClient` connections to the
  address the admin request arrived at (the local end of its connection, not
  the `Host` header, which names the development proxy). They send `player:*`
  events like the plugin, including `player:enter` before the first checkpoint,
  so the server's parsing, validation, relay, and per-connection delivery are
  exercised as well. They share the server's process and event loop, so their
  receive work counts against the server's CPU.

A bot that cannot connect, or whose connection drops, is reported as failed and
leaves the rest of its batch running.

### Admin state

Bots are not listed in the snapshot's `players`. Hundreds of rows would bloat
every snapshot pushed to the panel and bury the real players. Each batch lists
its bots with status, offset, rate, and position; the panel extrapolates
positions between pushes the same way it does for replays.

Bots are named after the recorded player with their number (`Lydia #3`), so
they can be told apart in game and in `latest.log`.
