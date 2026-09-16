## Project overview
This project implements non-intrusive multiplayer for Skyrim where only the player actions, movement, animations 
are synced which do not affect the other players' world. We do not aim of any kind of loot, quest state, enemies synchronization.
Each player plays in its own world state seeing other players from their worlds.
We should attempt to be highly compatible with other player modpacks so different modpacks are compatible. 
We always should take vanilla animations, behaviors, capabilities as a baseline and fallback to them when there's mismatch
in installed mods.

## Build notes
- This project uses XMake under wine, so do not try to build it when the host is Linux.

## Debugging
- When investigating a bug, check `latest.log` first. It contains logs from the
  most recent run, which may include a reproduction. References to "the log
  file" usually mean `latest.log`.

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
