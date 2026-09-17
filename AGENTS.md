## Project overview
This project adds non-intrusive multiplayer to Skyrim. Each player keeps an
independent world and sees other players as remote actors. The plugin shares
player presentation: position, movement and animations, actions, appearance,
and equipped items. Remote players' actions should not change another player's
world. Loot, quests, enemies, and other world state are outside the sync scope.

Players should be able to use different modpacks. Treat vanilla animations,
behaviors, and capabilities as the common baseline, and fall back to them when
a peer does not have a matching modded asset or behavior.

## Project structure
- Follow the existing domain boundaries when adding features: `src/runtime/` for
  engine hooks and startup, `src/protocol/` for shared state and wire types,
  `src/network/` for transport, `src/sync/<feature>/` for synchronization logic,
  `src/actor/` for Skyrim actor behavior, and `src/ui/` for presentation. Keep
  server-side behavior in `server/`.
- Split a feature across the domains it touches, with each responsibility in its
  owning module. Keep coordination and entry points focused on ordering and
  delegation; avoid growing them into feature implementations.
- Prefer cohesive modules with narrow interfaces. Extract code when it has a
  distinct responsibility, rather than creating a new file for every helper.

## Build notes
- This project uses XMake under wine, so do not try to build it when the host is Linux.
- `xmake.lua` is the source of truth for the C++23 plugin target and dependencies.
  Follow the formatting and naming conventions in nearby project code; avoid
  formatting unrelated files or applying rules from vendored `lib/` code.

## C++ and engine boundaries
- Make ownership and lifetime explicit. Prefer scoped objects and RAII for new
  resources; avoid retaining raw Skyrim object pointers or references across
  callbacks, disconnects, or actor respawns without checking that they are valid.
- Keep Skyrim engine reads and mutations on the authoritative main-loop thread.
  Network callbacks should validate and queue data; apply it through the sync
  path on the game thread. Use `GameThread::Require` at new remote-actor engine
  entry points.
- Keep headers focused on the interface they expose. Include what a header uses
  directly, and keep implementation-only dependencies in `.cpp` files when
  possible.

## Protocol and verification
- A wire change usually spans `src/protocol/`, serialization and parsing in
  `src/network/`, validation and relay in `server/src/server/`, and the owning
  `src/sync/` feature. Check all of these and update the matching server tests.
- Validate untrusted network fields at the boundary, including types, finite
  numbers, ranges, sequence or revision ordering, and form references. Preserve
  documented defaults for optional fields so older recordings and different
  client versions can still be read where possible.
- For server changes, run the relevant test module from `server/` with
  `uv run python -m unittest discover -s tests -p 'test_<module>.py'`.
  Run the full suite with `uv run python -m unittest discover -s tests` when
  protocol behavior changes.
  For plugin changes on Linux, report that the Windows XMake build and in-game
  behavior were not verified, and describe any checks that were possible.

## Debugging
- This plugin relies on reverse-engineered Skyrim behavior. When logs and
  source code leave an engine behavior unclear, inspect the relevant game
  binary in a disassembler to check call paths, data layout, and assumptions
  before changing a hook or actor behavior.
- Runtime inspection is limited, so logs are one of the main ways to debug the
  plugin. Check `latest.log` first when investigating a bug: it contains the
  most recent run and may be the only capture of a reproduction. References to
  "the log file" usually mean `latest.log`.
- Logging is primarily written by agents for agents. If the existing log does
  not show enough to explain a failure, add targeted instrumentation to the
  relevant code and use the next runtime capture to test the hypothesis. Log
  useful state changes and context without flooding the log every frame.

## Timing and frame rate
- Peers run at different and variable frame rates, so never express a duration
  as a number of frames. Timeouts, fallbacks, observation windows, retry budgets,
  cooldowns, and interpolation must accumulate the frame delta in seconds (clamp
  it, as `kMaximumFrameTime` does) and use `...Seconds` constants.
- A retry budget may keep an attempt count, but must also require a minimum
  elapsed time, so a high frame rate cannot spend every attempt within a few
  milliseconds.
- Frame counts are acceptable only when the logic really means engine update
  passes (for example "stable across two main-loop passes" or "give the graph one
  update"), or for log throttling and diagnostics. Say so in a comment.
