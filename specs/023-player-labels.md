# Player Labels

## Status

Implemented, pending a Windows build and in-game check. The server's name
indexing, level validation, and chat fields are covered by unit tests.

## Goal

Name every player the same way on its remote actor and in chat, with its level,
and tell apart players who use the same character name:

```text
Lydia • 12 lvl
Lydia #2 • 101 lvl
```

## Wire

- `player:appearance` from the client carries `level` next to `name`
  (`specs/006-player-appearance-sync.md`). The client captures it with
  `Actor::GetLevel`, so a level-up publishes a new revision.
- The server relays and replays every appearance with a `nameIndex`, and
  `chat:message` carries the sender's `name`, `nameIndex`, and `level`
  (`specs/020-player-chat.md`).

## Name index

`PlayerRegistry.set_appearance` assigns the index when a player's first
appearance arrives or its name changes. Names are compared case-insensitively.
The player gets the lowest index from 1 not held by another stored player with
that name, and keeps it while the name stays the same, so indexes are never
renumbered when a player leaves. Replayed and load-test players are indexed the
same way. The index is not part of the stored appearance, so recordings do not
contain it.

## Label

`PlayerLabel::Format` builds `Name #N • L lvl`:

- An empty name is shown as `Unknown`.
- `#N` is shown only for an index above 1, so a unique name has no suffix.
- The level part is shown only for a known (non-zero) level.
- The bullet is U+2022, written as UTF-8 bytes.

The remote actor base's full name is the label. When a new appearance differs
from the previous one only in name, level, or index, the receiver rewrites the
built base's full name in place and adopts the revision without building a new
base or respawning the actor.

## Verification

- Server: `test_app_appearance.py` covers level validation, missing and
  `null` levels, and name index assignment, reuse, and late-join replay.
  `test_app_chat.py` covers the chat fields; `test_admin.py` covers replayed
  players.
- In game (pending): two players with the same name show `Name` and
  `Name #2` over their actors and in chat; a level-up updates the remote label
  without a respawn; the crosshair font renders the bullet.
