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
