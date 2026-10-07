## Project overview
This project adds non-intrusive multiplayer to Skyrim. Each player keeps an
independent world and sees other players as remote actors. The plugin shares
player presentation: position, movement and animations, actions, appearance,
and equipped items. Remote players' actions should not change another player's
world. Loot, quests, enemies, and other world state are outside the sync scope.
The one exception is an item trade both players accept, which changes only each
player's own inventory (`specs/trade.md`).

Players should be able to use different modpacks. Treat vanilla animations,
behaviors, and capabilities as the common baseline, and fall back to them when
a peer does not have a matching modded asset or behavior.

## Project structure
- Follow the existing domain boundaries when adding features: `src/runtime/` for
  engine hooks and startup, `src/protocol/` for shared state and wire types,
  `src/network/` for transport, `src/sync/<feature>/` for synchronization logic,
  `src/actor/` for Skyrim actor behavior, and `src/ui/` for presentation. Keep
  server-side behavior in `server/` (Go) and the wire schema in `proto/`.
- Split a feature across the domains it touches, with each responsibility in its
  owning module. Keep coordination and entry points focused on ordering and
  delegation; avoid growing them into feature implementations.
- Prefer cohesive modules with narrow interfaces. Extract code when it has a
  distinct responsibility, rather than creating a new file for every helper.

## Specs
- `specs/<system>.md` is a retroactive summary of the implementation,
  organized by system, so humans and agents can pick up context quickly. A
  system is a part of the project with its own state, responsibilities, and
  interfaces, such as the remote actor, the sync pipeline, the network, or the
  UI overlay. It usually maps to one or a few source domains. `specs/README.md`
  lists the systems and maps each feature to the specs that describe it.
- A system spec describes its components and their responsibilities, its
  interfaces and data flow with other systems, its ordering and invariants, and
  the features it implements. A feature that spans systems is described in each
  system's spec for that system's part only. The system that owns the feature's
  behavior holds its goal, non-goals, and acceptance criteria and links to the
  other parts. Every wire message and field, with its validation, lives in
  `specs/network.md`; for example, chat's messages and relay are there, and its
  window is in `specs/ui-overlay.md`.
- Read the specs of the systems you change before changing them. The code is
  the authority when a spec disagrees with it; fix the spec when you notice the
  difference.
- Keep specs current in the same change as the code. Update every system spec
  whose described behavior the change affects, and `specs/README.md` when a
  system or feature is added, split, or removed. Add a spec only for a new
  system; a new feature extends the specs of the systems it touches.
- Describe the current state, not its history. Replace outdated sections rather
  than appending changes. Mention a dropped approach only where it explains a
  decision, such as a non-goal, and briefly say why it was dropped.
- Structure a system spec as `Status`, `Scope` (what the system owns, what it
  leaves to other systems, and how they connect), `Components` (modules and
  their responsibilities), one section per feature or concern,
  `Engine assumptions`, `Acceptance criteria`, and `Known limitations` (with
  non-goals). Keep only sections with content.
- `Status` states what was actually verified and how: server unit tests, Linux
  compile or headless checks, Windows build, in game and on which runtime. Give
  a feature its own status where it differs from the system's. Keep unverified
  engine behavior under `Engine assumptions`.
- Keep specs precise and to the point. Name wire fields, validation rules and
  defaults, code symbols, config keys, form IDs, and engine functions; state
  decisions with their reasons. Leave out line numbers, code listings, and
  details that are obvious from the code. Link to another spec's section rather
  than repeating it.

## Code style guidelines
- Comment only what a reader cannot get from the code:
  - non-obvious constraints and "why" decisions at that spot in the code, such
    as ordering, locking, and lifetime requirements;
  - engine or third-party behavior that cannot be derived from the code, such
    as reverse-engineered Skyrim behavior, Address Library IDs, and library
    quirks;
  - the frame-count notes required below.
  Do not comment self-evident "what". Document the plugin's and server's own
  behavior, contracts, and system-level design decisions in the system specs,
  not in doc comments.

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

## Performance
- Keep the client modification performant: expect hundreds of remote actors to
  be synchronized at once. Treat per-actor, per-frame work as the hot path and
  consider how a change scales with the number of remote actors.
- Avoid per-frame allocations, repeated engine lookups, and linear scans over
  all actors where a cached or indexed lookup would do. Do expensive work only
  when state changes, and spread or throttle work that does not need to run
  every frame.
- Profile with Tracy by configuring `xmake f --tracy=y`. Instrument with the
  macros in `src/runtime/Profiling.h`, which expand to nothing in normal builds.
  Prefer one zone per sync stage or per-peer function over zones in tight loops
  or high-frequency engine hooks.

## Protocol and verification
- Backward compatibility between protocol or server versions is not a priority:
  every client release is supplied with its required server update. Prefer clear,
  direct protocol evolution over compatibility shims for older client versions.
- A wire change usually spans the schema in `proto/playerhub/v1/`, plain values
  and quantization in `src/protocol/`, encoding and parsing in
  `src/network/WireCodec`, validation in `server/internal/validate`, ordering
  and relay in `server/internal/hub`, and the owning `src/sync/` feature. Check
  all of these and update the matching server tests.
- After editing a `.proto` or `.options` file, run `proto/generate.sh` from a
  Linux or Windows shell (never under wine) and commit the regenerated Go and
  nanopb code with the change.
- Validate untrusted network fields at the boundary, including types, finite
  numbers, ranges, sequence or revision ordering, and form references.
- For server changes, run `go vet ./...` and the relevant package's tests from
  `server/` with `go test ./internal/<package>`. Run `go test -race ./...` when
  protocol or concurrency behavior changes, and `cmd/loadgen` against a local
  server (port 3100) when a change may affect throughput or latency.
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
  it with `FrameTime::Clamp`) and use `...Seconds` constants.
- A retry budget may keep an attempt count, but must also require a minimum
  elapsed time, so a high frame rate cannot spend every attempt within a few
  milliseconds.
- Frame counts are acceptable only when the logic really means engine update
  passes (for example "stable across two main-loop passes" or "give the graph one
  update"), or for log throttling and diagnostics. Say so in a comment.
