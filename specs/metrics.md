# Metrics

## Scope

The server keeps its own history: which sessions connected and for how long,
why connections ended or were refused, and once a minute how many players were
online, the WebSocket traffic, and the hub loop's load. The admin panel's
Statistics page reads it as a report over a range ([Panel pages](recording-and-replay.md#panel-pages)).

Goal: an operator sees how many people play, when, for how long, whether they
come back, and how the server held up, without outside tooling. Non-goals:
anonymized history (the connection log keeps session IDs and display names, so
the page can rank players and measure returns), voice traffic, per-player
traffic history, alerting, and export to Prometheus or similar.

The session server ([Server](network.md#server)) reports connections and
refusals; the hub loop measures its own work; `cmd/server` wires both into the
recorder and gives the admin API the store.

## Components

| Module | Responsibility |
| --- | --- |
| `internal/metrics` `Store` | `metrics.db` schema, writes, rollups, purging, `Report` |
| `internal/metrics` `Recorder` | Queues connection events and counts refusals off the callers' goroutines, writes them and a sample a minute on its own |
| `session.Server` | Calls the recorder, classifies how a connection ended, counts payload bytes (`Traffic`) |
| `hub.Loop.Load` | Time the loop spent working since start, and its longest pass since the last read |
| `admin` `statistics` | `GET /api/statistics` |
| `admin-ui/src/features/statistics` | The Statistics page |

## Recording

- **Connections:** `Recorder.Connected` runs once a joined player is in the
  hub and returns an ID the recorder assigns (continuing from the stored
  maximum); `Recorder.Ended` runs as the connection's goroutine finishes, with
  the end time and reason. Both only queue (4096 events); a full queue drops the
  event and logs a warning with the count at the next write. Events are written
  in one transaction a second.
- **End reasons**, the first one given wins (`connection.end`): `kicked`,
  `banned`, `replaced` from the close status the server sent (4011, 4010, 4006;
  deleting a session closes as a kick); `timeout` when a liveness ping hits its
  own deadline; `rate_limited` (4003); `backlog` (4002); then
  `server_stopped` while the server shuts down; `left` when the client sent a
  close frame; otherwise `dropped`. A recorder that starts finds connections a
  previous run left open and ends them as `server_lost` at the end of its last
  sample; a recorder that stops ends the rest as `server_stopped`.
- **Players:** a session's first connection adds it to `players` with that
  time. A newly created database first copies every kept session's
  `created_at` from the session store, so new players per day reach back to the
  oldest session the store kept.
- **Refusals** are counted in memory per reason and added to their hour with
  each sample. A WebSocket refusal takes its reason from the close status
  ([Transport](network.md#transport)): `protocol_mismatch` (4001),
  `display_name_taken` (4004), `display_name_invalid` (4005), `server_full`
  (4007), `too_many_new_sessions` (4008), `address_full` (4009), `banned`
  (4010), `invalid_hello` (1008), `server_error` (1011). A connection that
  sends no hello before `limits.hello_timeout_seconds` or closes first is
  `no_hello`; an upgrade refused by the pending hello caps (429/503) is
  `joining_busy`.
- **Samples:** at each whole minute the recorder reads `Gauges` through a probe
  that counts `KindClient` players in the hub and those in the world (one
  `Loop.Call`), `session.Server.Traffic`, and `Loop.Load`. A row keeps the
  window's length in seconds, the player counts, payload bytes read and
  written, loop busy time, and the longest pass. Windows shorter than 5 s
  (just after start) are folded into the next; a failed probe skips one. On
  stop it writes the partial window, or only the refusals when it is shorter.
- `Loop.Load` times each submitted function, `Tick`, and `UpdateInterest`; the
  longest pass is kept with compare-and-swap so the reader's reset is not lost.

## Storage

`[metrics] database` (default `metrics.db` in the working directory; empty
keeps it in memory), SQLite in WAL mode with one connection, `user_version` 1;
`meta.created_at` records when it was made.

| Table | Rows |
| --- | --- |
| `connections` | `id`, `session_id`, `display_name`, `started_at`, `ended_at` (null while open), `end_reason` |
| `players` | `session_id`, `first_seen_at` |
| `samples` | One per minute, keyed by the minute it starts |
| `hourly` | Each hour's sums and maxima over its samples, rewritten with every sample |
| `refusals` | Count per hour and reason |

Times are Unix milliseconds. Every hour the recorder deletes samples older
than 14 days, and hourly rows, refusals, and ended connections older than
`[metrics] retention_days` (default 365, at least 1). Players are kept, so a
returning session is never counted as new.

## Report

`GET /api/statistics?range=&tz=`: `range` is `24h`, `7d` (default), `30d`,
`90d`, or `all`; `tz` an IANA zone name (default UTC; the server embeds
`time/tzdata`); anything else is 422. Days, hours, and weekdays are in `tz`.
The answer is `metrics.Report` plus `uplinkBytesPerSecond`
(`budget.uplink_bytes_per_second` while the budget is on, else null).

| Range | Starts at | Fine buckets | Coarse buckets |
| --- | --- | --- | --- |
| `24h` | The hour 23 hours ago | 10 min, from samples | 1 h |
| `7d`, `30d`, `90d` | Local midnight 6, 29, or 89 days ago | 1 h, 3 h, 6 h, from hourly rows | 1 day |
| `all` | The earliest row of any table | Those of the shortest range covering it | |

Buckets end at the next start or at now. Steps of hours align to the local day
and advance by local clock, so a day with a daylight saving change keeps local
midnights.

- **Fine series:** average and peak players, average players in the world,
  average and peak-minute bytes per second out, average in, the loop's busy
  share, and its longest pass. A bucket without samples is null.
- **Coarse series:** different sessions connected in the bucket (`unique`),
  sessions first seen in it (`new`), connected sessions not first seen in it
  (`returning`), and the peak.
- **Summary:** the peak and its time, unique players, players first seen in the
  range (which includes copied sessions without connections in the log),
  connected players not first seen in it, play time (connections clipped to the
  range, open ones to now), finished play sessions and their median length,
  and the return shares.
- **Play sessions** join a session's connections less than 5 minutes apart
  (`playSessionGap`), so reconnects and replacements count once. Only those
  started in the range and finished count toward the lengths, in buckets with
  upper bounds of 5, 15, 30, 60, 120, and 240 minutes and an open last one.
- **Return shares:** of the players first seen in the range, and after the
  database was created, those whose next local day has passed are eligible for
  the next day, and those whose following 7 days have passed for the week; a
  player returned when a connection overlaps that span.
- **Heatmap:** average players per local weekday (Monday first) and hour over
  the range's sample or hourly rows.
- **Top players:** the 10 longest play times in the range with the latest
  display name and connection count. **Disconnects:** connections that ended
  in the range by reason. **Refusals:** per reason, a total and counts per
  coarse bucket.

The report reads the range's connections into memory (two indexed halves,
ended after the start and still open) and walks them once in Go, so its cost
grows with the connections overlapping the range: on a synthetic year of
368,000 connections from 8,000 players, 7 days took 16 ms, 90 days 240 ms, and
`all` about a second.

## Statistics page

`features/statistics/StatisticsPage`: a range control in the address
(`?range=`, `7d` when absent) above everything, stat tiles, then charts. It
fetches on range change and every 60 s and keeps the last report dimmed while
the next loads. It is not in the event feed snapshot.

- The online tile counts the snapshot's connected players; the rest come from
  the summary.
- `TimeChart` wraps uPlot: lines with a crosshair tooltip listing every series
  in the bucket, or bars stacked by drawing running totals tallest first. Its
  y axis starts at zero; a reference line (the uplink) widens the axis only
  when it is within twice the data's maximum, and the legend carries its
  value. Each chart's panel switches to a table of its buckets.
- Players online and players per day (stacked new and returning) for the
  population; `Heatmap` with a readout of the hovered or busiest cell;
  session lengths, refusal and disconnect reasons as `BarList`s; the top
  players link to the Sessions page searching for their session ID.
- Under server health: bandwidth with the uplink, the loop's busy share and
  longest pass as separate charts (no shared axis), refused joins per bucket
  with their reasons, and disconnect reasons.
- Series colors are the `--series-1..3` tokens in a fixed order, the heatmap
  ramp `--heat-low`/`--heat-high`; `useChartTheme` resolves them for the canvas
  and follows the color scheme.

## Acceptance criteria

- A player who connects, is kicked, connects again, and leaves appears as two
  connections ending `kicked` and `left`, one unique player, and one new player
  on the day of their first connection.
- A refused hello adds one to its reason in that hour, also when the server
  stops before the next sample.
- A server killed without shutdown has its open connections ended as
  `server_lost` at its last sample on the next start.
- Day buckets start at local midnight in the panel's zone, across daylight
  saving changes.

## Known limitations

- `cmd/loadgen` bots are real clients and count as players; virtual players
  and in-server batches do not.
- Traffic counts WebSocket payload bytes, not voice datagrams or framing, so it
  is below what the uplink carries.
- Players first seen before the database existed have no connections in the
  log: they count as new on their creation day, but not as unique, and not
  toward return shares. Sessions the store purged before then are not counted.
- The hour of a sample is its UTC hour, so in zones offset by a fraction of an
  hour the heatmap and the longer ranges' buckets are off by that fraction.
- Events beyond the 4096 queued, or written after the recorder stopped, are
  lost.
