# Player Death

## Status

Implemented. The server side is covered by unit tests. In game, a replayed
player's deaths, standing and while seated, collapsed the remote actor into a
ragdoll that the next `player:exit` removed. The remaining items under
[Engine assumptions](#engine-assumptions) and
[Acceptance criteria](#acceptance-criteria) are not yet checked individually.

## Goal

When a player dies, peers see the remote actor die the way a vanilla NPC does:
it collapses into a ragdoll and the body lies where physics leaves it. Vanilla
death ends in a load (the last save or the main menu), and that load's
`player:exit` removes the body, so peers see it for the few seconds of the
death camera.

## Non-goals

- Who or what killed the player, kill moves, and death animations. The actor
  always goes straight to ragdoll.
- Matching where the body comes to rest. Each peer simulates its own ragdoll.
- Bleedout and an essential player's collapse. They are not death: the player
  gets up again without a load.
- Looting the body. Remote actors are spawned with activation blocked.

## Protocol

`player:update` and `player:state` carry `dead`, a boolean that is true while
the player's life state is dying or dead. It is checkpoint state, not an
action: a peer that joins, respawns the actor, or re-enters the space while the
player is dead still sees a body. The server validates it as a boolean and
defaults it to `false` when missing, so recordings made before this change
replay as alive. Every relayed `player:state` includes it, and the client
requires it.

## Source capture

`DeathSync::SampleLocalDeath` reads the local player's `ACTOR_LIFE_STATE` once
per published checkpoint. `kDying` and `kDead` publish `dead`; every other state,
including `kBleedout` and `kEssentialDown`, publishes alive. Checkpoints keep
flowing while the player is dead, until the load's `player:exit`.

## Receiver

`DeathSync::SyncDeath` runs in the remote pass after the coordinate-space
checks and before every stage that drives the actor:

1. A live actor whose newest checkpoint is dead is killed once it is ready (3D,
   controller, and graph). An actor spawned for a player that is already dead
   therefore stands for the few passes its initialization takes, then
   collapses where it stands. Queued attacks are dropped.
2. A killed actor is left alone: movement, animation, furniture, equipment,
   combat, and the stranded-actor check are skipped. Only its handle is
   watched (a lost handle respawns it) and its appearance presentation is kept.
3. A killed actor whose newest checkpoint is alive again (for example a
   console `resurrect`, or a mod that revives the player) is retired and
   respawned standing at the checkpoint.

`ActorDeath::Kill` hands the actor to Skyrim:

- Its furniture is released first, as on retirement: occupancy, marker
  reservations, and the hold published to the local player's activation. A
  body is no longer driven out of a chair, and would otherwise keep it from
  the local player.
- `ActorRegistry::SetDead` marks the registration dead. From then on the
  `KillImpl`, `KillDying`, `HandleHealthDamage`, and health `ModActorValue`
  guards let calls through, the engine update hook no longer reasserts the
  survival policy (which would resurrect the actor), and the animation
  firewall admits every graph event, as it does for native furniture.
- The actor's `kEssential` and `kProtected` flags are cleared.
- `Actor::KillImpl(nullptr, 0, false, true)` kills it with no attacker and no
  death event, so no local quest, script, crime, or statistic observes the
  death, and ragdolls it immediately.

The registration is per actor reference, so a respawned actor starts protected
again.

## Engine assumptions

- `KillImpl` with `a_sendEvent = false` still performs the full death: life
  state, ragdoll, and the actor stopping its AI.
- Clearing `kEssential` is enough for `KillImpl` to kill an actor spawned from
  a remote player's base.
- A dead temporary reference keeps a valid handle while its cell stays loaded.
  If Skyrim cleans it up, the body is respawned and killed again.
- The player's life state goes through `kDying`/`kDead` on a vanilla death
  before the load opens.

## Acceptance criteria

- A player killed by an enemy collapses into a ragdoll on every peer, and the
  body disappears when the dead player's game loads.
- The body cannot be looted, and the local player gains no kill, bounty, or
  quest progress from it.
- A peer that enters the dead player's cell during the death camera sees the
  body collapse where the player died.
- A player revived with `resurrect` stands up again on peers as a fresh actor.
- A player in bleedout (essential) stays standing on peers.
