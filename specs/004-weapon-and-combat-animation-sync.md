# Weapon and Combat Animation Synchronization

## Status

Implemented for drawn/sheathed presentation and the first semantic combat
action: a normal weapon attack. Spec 012 extends the vocabulary with left-hand and
dual-wield attacks and bashes, and adds blocking as checkpoint state. Spec 013
presents power attacks, which this spec originally showed as normal attacks.
Equipment selection, damage, targeting, and general combat behavior remain out
of scope.

## Goal

Extend remote actor animation synchronization beyond locomotion with an
ordered, reconstructable animation-action stream. Weapon draw and sheathe are
the first action family; attacks and later combat presentation build on the
same lifecycle rather than being folded into movement snapshots.

## Scope

- Replicate the current weapon presentation as an orthogonal `weaponDrawn`
  checkpoint field.
- Let Skyrim perform the native draw/sheathe transition on the actor.
- Preserve actor ownership of combat decisions, targets, damage, casting, and
  locally generated graph events.
- Replicate accepted normal and power attack starts through one ordered,
  versioned `attack/normal` action.
- Gate attack playback behind graph epoch, checkpoint, draw/equip, jump, and
  prior-attack readiness without stopping walk/run locomotion.

## Current weapon-draw behavior

The source samples whether its weapon is drawn and includes `weaponDrawn` in
the latest player state. Like locomotion and jump phase, this is current state,
not action history: the receiver retains only the newest checkpoint per remote
player.

On the game thread, the actor compares the authoritative desired state with
Skyrim's current weapon state. When a transition is needed and a matching
native transition is not already in progress, it invokes Skyrim's native
draw/sheathe path. A actor-only animation firewall continues to reject local
animation injection, except for presentation-only events managed by the
client. Skyrim's `WantToDraw` and `WantToSheathe` states only represent an
outstanding request, so the receiver retries the native request until the actor
advances to `Drawing` or `Sheathing`. Native weapon transitions require:

- `WeapEquip`
- `WeapUnequip`
- `Equip`
- `Unequip`

Skyrim also manages the non-interrupting swimming transitions locally, so the
firewall permits `SwimStart` and `swimStop`. The vanilla `Swim_Behavior`
enters `SwimIdle` or `SwimLocomotion` from `iSyncIdleLocomotion`, which stays 0
on a remote actor, and leaves `SwimIdle` only on `moveStart`. When `SwimStart`
is accepted while the dispatched and target locomotion are both moving, the
synchronization pass waits two main-loop passes for the swim branch, then sends
`moveStart` until it is accepted, the actor stops moving or leaves the water,
or 0.5 seconds pass. A swim started while standing needs nothing extra: the
later idle-to-moving dispatch sends its own `moveStart` into `SwimIdle`. After
`swimStop` is accepted and the actor leaves Skyrim's swimming state, the
synchronization pass reasserts the current locomotion events so a moving actor
returns to walking instead of sliding in an idle pose.

`SwimStart`, `JumpLand`, and `JumpLandEnd` are all wildcard transitions of the
vanilla root state machine, and `JumpLandEnd` leads to `Default_State` (land
locomotion) from `Swim_State` too. A jump into water reaches the network as a
landing at the surface, and Skyrim sends `SwimStart` just after it (observed in
game), so the landing clip never emits `JumpLandEnd`. While Skyrim reports the
actor swimming, a pending landing therefore ends without the bounded
`JumpLandEnd` fallback or the grounded locomotion reassert, and a network
landing that arrives after `SwimStart` sends no `JumpLand`. Otherwise the actor
walks on the riverbed until it leaves the water.

Weapon clips similarly take temporary ownership of the graph. The receiver
waits for the matching `WeapEquip_Out[Moving]` or `Unequip_Out[Moving]`
annotation and allows its worker-thread exit one graph update to settle. After
the base locomotion checkpoint has settled, a moving draw/sheathe request is
queued before its transition-specific `moveStart`; the draw graph selects its
standing or moving branch at entry, so deferring that edge until `ActorState`
reports `Drawing` can expose the standing clip while the actor keeps moving.
After unequip, when both the graph and actor state should be sheathed, the
receiver forces `IdleForceDefaultState` before replaying the current locomotion
and jump checkpoint. It must not force the default state after equip because
that would desynchronize the graph's sheathed state from the actor's drawn state;
`weaponDraw` and `weaponSheathe` only mark the visual attachment change and do
not close the clip branch. The replay remains pending if the transition began
while idle, so movement that starts after the clip is still restored. Compound
states are restored in graph-valid stages: base movement precedes sprint, sneak
movement follows sneak idle, and takeoff follows grounded locomotion. A bounded
fallback sends the matching equip/unequip exit event when a custom graph omits
its completion annotation. Weapon recovery does not hold newer movement
checkpoints or network translation behind a recovery loop.

This exception is intentionally narrow. It does not allow local attack, hit,
cast, combat-target, or AI-driven animation events to take graph ownership.

## Normal-attack action stream

`weaponDrawn` cannot express attacks: a latest-wins boolean loses repeated or
short-lived actions. Each semantic action family therefore needs a separate,
ordered action stream in addition to the latest movement checkpoint.

An action record must include at least:

```json
{
  "actionSequence": 1,
  "graphEpoch": 1,
  "checkpointSequence": 42,
  "family": "attack",
  "action": "normal",
  "prerequisites": {
    "weaponDrawn": true
  }
}
```

- `actionSequence` is monotonic per remote player and action stream. It makes
  duplicate and stale actions detectable without coupling action ordering to
  transform packet coalescing.
- `graphEpoch` increases whenever the source graph is rebuilt, reverted, or
  otherwise loses action-stream continuity. The sender/receiver handshake must
  re-establish a compatible checkpoint before dispatch resumes.
- `checkpointSequence` identifies a state checkpoint from the same graph epoch
  that was published before the action. This makes the drawn-weapon
  prerequisite reconstructable even though transforms themselves remain
  latest-wins.
- `family` and `action` select a deliberately supported, versioned vocabulary;
  arbitrary graph event names are never accepted from the network.
- `prerequisites` state the required reconstructed presentation state. An
  action must wait rather than firing into a graph that is not ready.

The main-loop sampler observes edges in PlayerCharacter's melee
`ATTACK_STATE_ENUM`, including a new swing after `NextAttack`/`FollowThrough`.
This is downstream of input mapping and works for both normal and power attacks;
spec 013 marks power attacks with `power` and a direction, and no native graph
event string crosses the network. The next 20 Hz checkpoint is emitted before the action and supplies its
graph epoch and checkpoint sequence.

The server validates the vocabulary, prerequisite, epoch, checkpoint, and
strictly contiguous action sequence, then relays the record as `player:action`.
Socket.IO preserves ordering within a connection. A duplicate, stale action, or
sequence gap is rejected rather than played out of order. The first observed
sequence establishes the boundary for a late-joining receiver; the server does
not retain historical actions.

The receiver holds a bounded per-player FIFO. Its front action waits until:

- its checkpoint has arrived and its graph epoch matches the latest checkpoint;
- the actor graph and latest applied animation state are settled;
- jump/landing and draw/equip recovery no longer own the graph;
- `ActorState` confirms the weapon is fully drawn; and
- the preceding synchronized attack has completed.

Once ready, the receiver briefly suspends only the actor's
`kAttackingDisabled` flag for the synchronous dispatch and sends the fixed
`attackStart` event through the network-owned firewall bypass. The flag is
restored before another actor/AI update can run. The receiver does not send
`moveStop`, zero speed, or pause translation, so Skyrim can select its native
walking/running attack branch.
Jump and equip checkpoints wait while an attack owns the graph, preventing them
from cutting off the accepted attack. Attack ownership ends when Skyrim's melee
attack state returns to `None`; bounded start/playback fallbacks prevent a
custom graph from holding the stream forever. Grounded locomotion is then
reasserted. A queued action times out after ten seconds, dispatch is retried at
most four times, and the FIFO is capped at 32 records.

## Reconstruction and late join

Late join, graph replacement, actor respawn, and recovery after a hit reaction
must first restore a current checkpoint: transform, locomotion, jump phase,
weapon stance, equipment identity when supported, and graph epoch. Historical
attacks are not replayed during reconstruction. Only actions following the
accepted checkpoint/epoch boundary may be dispatched.

## Non-goals

- Equipment inventory or visual loadout replication. That is specified in
  spec 007, which also gates draws and attacks on the synchronized weapon.
- Combat simulation, hit validation, damage, targeting, or perks. Bow draws and
  spell casting are presented by spec 011 as checkpoint state, and blocking and
  bashes by spec 012.
- Preserving power-attack presentation in this phase; spec 013 adds it.
- Replaying an attack that occurred before a late join or graph reconstruction.
- Allowing arbitrary remote behavior-graph events.

## Acceptance criteria

1. A remote actor converges to the authoritative drawn/sheathed state without
   enabling local combat AI or local attack events.
2. Repeated state packets and dropped intermediate draw/sheathe packets still
   converge to the latest weapon stance.
3. A graph rebuild restores the latest stance before any subsequently queued
   semantic action is considered dispatchable.
4. Every locally accepted normal attack produces one ordered remote normal
   attack. A locally accepted power attack produces one ordered remote power
   attack (spec 013).
5. An attack waits for jump/landing and draw/equip completion and cannot
   interrupt those graph branches. A later jump, equip transition, or attack
   does not interrupt an already accepted synchronized attack.
6. Walking and running actors keep locomotion and translation active while the
   normal attack plays.
7. Only the `attack` vocabulary (`normal`, and `left`, `dual`, and `bash` from
   spec 012, with the `power` and `direction` fields of spec 013) is accepted, with contiguous action
   sequence, matching graph epoch, a prior checkpoint, a drawn-weapon
   prerequisite, bounded queue/retries, and explicit timeout/rejection results.
