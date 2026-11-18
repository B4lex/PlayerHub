# Idle Presentation

## Scope

A remote actor plays the vanilla idles its source plays: gestures, dances,
sitting cross-legged, leaning, kneeling, meditating, instruments, and any other
`Idle*` graph event the source's graph accepts, whether from the emote mod
"Idle Animations Wheel Menu SSE" (`IdlePlayWheelMenu.esp`), the console,
scripts, or other mods. Each viewer sends the event to the remote actor's own
behavior graph, so a viewer's idle replacer applies. No pose crosses the wire,
and nothing in the viewer's world changes. Modded `Idle*` events, which no
vanilla behavior lists, are shown through
[pose windows](remote-actor.md#source-window) instead.

This system owns source capture of idle events, the source's held idle, the
receiver's idle queue, idle ownership of the remote graph, and release and
re-entry. Wire fields are in [Network](network.md#actions) and
[Network](network.md#checkpoint-and-context). Furniture idles, mounts, and death
belong to [Native interactions](native-interactions.md), beast forms to
[Combat presentation](combat-presentation.md#beast-actions).
[Locomotion](locomotion.md) and [Combat presentation](combat-presentation.md)
share the graph and hold their writes while an idle owns it
([Ownership](#ownership)).

Idle names, transitions, and clip annotations come from the vanilla behavior
files in `Skyrim - Animations.bsa` (`0_master.hkx`, `mt_behavior.hkx`,
`idlebehavior.hkx`, `1hm_behavior.hkx`, first and third person).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/idle/IdleSync` | Source capture, held idle, publishing; receiver queue, held-idle state, release, re-entry |
| `actor/IdlePresentation` | Start gating (`CanStart`), `Dispatch`, `Release`, graph ownership |
| `actor/GraphStance` | The idle axis of the graph reading ([Graph reading](#graph-reading)) |
| `runtime/PlayerGraphEventHook` | The local player's accepted graph input events |
| `protocol/IdleEvent.h` | `Multiplayer::IdleEvent` and the name rules both ends apply |

## State and actions

- **Ordered actions**: every idle event the source's graph accepts is an `idle`
  `Action`, anchored to a checkpoint and graph epoch like an attack
  ([Combat presentation](combat-presentation.md#action-stream)), so entering,
  exit, and stop events keep their order.
- **Latest-wins state**: `Context.idle` names the event that entered the idle
  the source is held in, empty for none. A peer that missed the action (late
  join, respawn, recovery, dropped action) re-enters from it
  ([Re-entry](#re-entry)). A one-shot gesture that ended is never held.

## Source

### Capture

`PlayerGraphEventHook` passes every event the player's graph accepts to
`IdleSync::ObserveLocalGraphEvent` on the raising thread. It ignores modded
events (`PoseWindow::IsModdedEvent`) and non-human players (an atomic the main
loop sets), and keeps names that start with `Idle` in any case or are
`OffsetStop`, pass the [name rules](network.md#actions), and are not
`IdleForceDefaultState` (the engine's reset that ends every idle) or
`IdlePlayer` (sent at furniture entry to reset the idle machine). Kept events
go into a fixed queue of 32 under a lock.

`IdleSync::QueueLocalIdles` drains the queue each pass. While furniture (an
occupied reference), a mount, death, swimming, midair, or a beast form owns the
player, every event is dropped and the held idle ends. Furniture occupancy is
set on activation, before the entry idle plays
([Native interactions](native-interactions.md#source)), and lasts until the
exit clip ends, so a chair's `IdlePlayer` and `IdleChairFrontEnter` are never
relayed. The sit/sleep state does not count: a `FurnitureState` idle such as
`IdleSitCrossLeggedEnter` sets it without occupying anything.

- An **exit** is an event whose name contains `Exit` or ends in `Stop` or
  `StopInstant` (`IdleChairExitStart`, `IdleStop`, `OffsetStop`, ...). It is
  queued and ends the held idle only while one is held, since the engine and
  scripts also stop idles peers never received.
- Any other event is **entering**: it is queued and becomes the held idle.
- `OffsetStop` ends the carry-arm overlay (`CarryLeftArmBehavior`,
  `CarryRightArmBehavior`) that some idle clips start through an annotation,
  such as `IdleMQ201Drink`'s `OffsetCarryMQ201DrinkR`; the emote mod sends it
  on its exits.
- At most 2 events a second are queued, burst 4; more are dropped and logged
  (`Idle sync dropped local idle event over the rate limit`). Idle actions share
  the server's action allowance with attacks, and an action the server drops
  leaves a sequence gap that rejects later actions.
- `CheckpointPublisher` samples the held idle with each checkpoint
  (`IdleSync::SampleLocalIdle`) and publishes queued events right after, anchored
  to it (`IdleSync::PublishLocalIdles`), so the context that first holds an idle
  and its action name the same checkpoint. An epoch change, a disconnect, or
  leaving the world discards queued events; a furniture or beast-form
  checkpoint publishes none.

### Held idle

The held idle ends, and the next context clears it, when:

- an exit is captured, or the drain finds the player blocked;
- a [pose window](remote-actor.md#source-window) opens (`pose-window`);
- the sampled checkpoint has furniture, a mount, death, swimming, or a beast
  form, or changed from the previous sample by a start of movement, a jump, a
  draw or sheathe, or a sneak toggle. A stop does not end it, since the
  published stop can trail the idle's start
  ([Locomotion](locomotion.md#source-capture));
- the [graph reading](#graph-reading), every 0.1 s, is back in the default
  state after showing an idle, or showed none within 0.5 s of the event (the
  machine switches on the graph update after the event). One-shot gestures end
  this way through their clip's `IdleStop` annotation.

A graph without any vanilla idle machine keeps the idle until an exit or a
state change. Logs: `Idle sync holding local idle`, `Idle sync ended local held
idle`, `Idle sync captured local idle event`, `Idle sync ignored local idle
event`, each with its reason.

## Receiver

### Queue

`IdleSync::AcceptRemoteAction` accepts an idle action with a known checkpoint,
an epoch no older than it, and no furniture into a FIFO of 32, interning the
name as a `BSFixedString`. Actions time out after 3 s; an epoch boundary drops
the old epoch's; retirement, a lost actor, mounting, and leaving the local space
clear the queue but not the held idle.

`IdleSync::SyncIdle` runs each pass for a human actor on foot outside
furniture, before attack and hand presentation. The front action waits for its
epoch and anchor checkpoint, then for a start the actor can take
(`IdlePresentation::CanStart`):

- the actor is ready, its locomotion revision settled, and no recovery,
  locomotion reassert, release settle, attack, hand presentation, or unsettled
  weapon transition owns the graph, which is the one the dispatch tracks;
- neither target nor checkpoint jumps or swims, and `WEAPON_STATE` is fully
  drawn or sheathed as the checkpoint says, so the emote mod's `unequip` before
  an emote plays its sheathe first
  ([Combat presentation](combat-presentation.md#weapon-stance));
- the hands reconciled the latest equipment revision, no attack is queued, and
  the checkpoint holds no block, bow draw, or cast.

`IdlePresentation::Dispatch` sends one event per pass through the firewall
bypass under `GameThread::Require`. A rejected event is retried at most 4 times,
0.25 s apart, then dropped with `Idle sync remote graph rejected idle event`
(throttled per peer), so a graph lacking the event shows nothing. An exit while
no idle plays is dropped. Accepted events log `Idle sync processed idle action`.

### Ownership

An accepted entering event makes the idle own the graph
(`IdlePresentation::IsPlaying`). `IdlePresentation::Advance`, at the start of
each locomotion update, reads the [graph](#graph-reading) every 0.1 s and ends
ownership once it is back in the default state after showing an idle, or showed
none within 0.5 s (`Idle sync remote idle ended`). An accepted exit keeps
ownership until its clip returns the graph. A graph without the vanilla idle
machines keeps ownership until a release.

While an idle owns the graph, nothing the plugin does on its own ends it:

- stance reconciliation and locomotion reasserts wait
  ([Locomotion](locomotion.md#stance-reconciliation)); furniture entry and idle
  starts wait for a release;
- the sitting state the idle sets is not unwound as a seat without occupancy
  ([Native interactions](native-interactions.md#exit)) until ownership ends;
- the weapon transition's `IdleForceDefaultState` cannot run, since an idle
  starts only after a transition settled and a draw or sheathe releases it;
- per-pass locomotion variables, the idle turn hold, and `turnStop` stay: no
  vanilla idle state reads them;
- the actor's own AI, packages, and scenes cannot start or end an idle, since
  the firewall rejects their events
  ([Remote actor](remote-actor.md#animation-firewall)).

### Release

`IdleSync::OnCheckpointPlayed` notes a start of movement, a jump, a draw or
sheathe, a sneak toggle, a start of swimming, or furniture in the played
checkpoints; `SyncIdle` releases on those, and on a queued attack or a held
block, bow draw, or cast. `IdlePresentation::Release` sends
`IdleForceDefaultState` past the firewall
(`AnimationFirewall::ForceDefaultGraphState`), which restarts the unarmed root
(`MT_RootBehavior`'s self-transition wildcard) and ends every unarmed idle, and
gives the graph one update before the next locomotion dispatch. That wildcard is
global, so from a drawn graph it would leave the weapon behavior while
`WEAPON_STATE` stays drawn, and nothing would bring the graph back. So while the
reading shows the drawn combat idle (`GraphStance::StanceSelectors::drawnIdle`:
`1HM_Behavior` in `CombatIdles`), the release sends `IdleStop`, which returns to
`1HM_Ready_State`. An entering idle anchored at or before the change is
dropped. Death, mounts, beast forms, and respawns replace the graph or its
update path instead.

When the held instance the actor was given ends without an exit the actor
accepted, and none is queued, the idle is released 1 s later (`held-ended`):
the source left it by itself, through events peers never receive, or the exit
timed out. 1 s exceeds how far the actor's idle trails its source's, so a
gesture ending on both graphs is not cut. Log: `Idle sync released remote idle`
with the reason and event.

### Re-entry

`OnCheckpointPlayed` follows the played checkpoints' `idle`. A change starts a
new instance, named by the sequence of the first checkpoint that showed it. The
graph records the instance it was given (`IdlePresentation::SetAppliedInstance`);
graph invalidation clears the record.

- An accepted or finally rejected action for the held idle records the current
  instance; the source anchors the action to the checkpoint whose context first
  holds the idle, so the two match.
- With no queued action, an instance the graph was not given is sent once held
  0.3 s, under the same start rules and retries (`Idle sync re-entered held idle
  from state`). This covers late joiners, respawns, graph replacement, hit
  recovery, and dropped actions, and never replays an ended gesture. The idle
  plays from its start.

## Graph reading

`GraphStance::ReadStanceSelectors` reports an `idle` axis from the vanilla
machines the `Idle*` events enter: on while any is in an idle state, off while
all active ones are in their default state, none when none is active. The root
machines stay in their default state while a nested machine plays an idle.

| Machine | Default | Idle states |
| --- | --- | --- |
| `MT_RootBehavior` (unarmed root) | `MT_State` | every other state: furniture-style idles (`FurnitureState`), instruments, salutes, dances (`NonOffsetIdles`), chairs, beds, `IdleStopIdles`, ... |
| `MT_BehaviorGraph` | `MT_Default_State` | `InteractionObjectState` (gestures), `MT_BoyRitualState` |
| `MT_Idle_3rdPersonBehavior` (dialogue idles) | `InitialIdleState` | every other state |
| `CarryLeftArmBehavior`, `CarryRightArmBehavior` | `MTLeftArmState`, `MTRightArmState` | every other state |
| `1HM_Behavior` | `1HM_Ready_State` | `CombatIdles` |
| `RootBehaviorGraph` | `Default_State` | `FullBodyPlayerIdles`, `StandingVampireFeed` |
| `Master_Behavior` | `Root` | `IdleDLC2TentacleWarpBook` |

A gesture returns to default through its clip's `IdleStop` annotation, a held
idle through its exit clip's `IdleFurnitureExit`. None of these names occur in
the werewolf or vampire lord graphs.

## Engine assumptions

1. A non-player actor's graph accepts the vanilla idle events the player's
   accepts and plays the same idle: the transitions carry no `IsNPC` condition.
2. Instrument and prop idles attach their props from clip annotations
   (`AnimObjDraw` and related), which reach the actor as graph output events.
3. A machine's state switches on the graph update after an accepted event, and
   `IdleForceDefaultState` returns every unarmed idle, furniture-style ones
   included, to default.
4. Outside furniture, the engine and scripts send the player an entering
   `Idle*` event only for an idle it plays.

## Acceptance criteria

1. A gesture plays once on peers, through each peer's own graph and replacers.
2. Sitting cross-legged, leaning, kneeling, meditating, playing an instrument
   (with its prop), and dancing hold on peers until the source exits, and the
   exit clip plays.
3. A peer that joins, or whose actor respawns or recovers, while the source is
   held in an idle sees it; an ended gesture is not replayed.
4. An emote started with the weapon drawn plays the sheathe first.
5. Moving, jumping, drawing, sneaking, or sitting in furniture ends the idle on
   peers without sliding in the idle pose.
6. Chair furniture plays only the furniture's own idle on peers.
7. An idle event a peer's graph lacks shows nothing and logs
   `Idle sync remote graph rejected idle event`.

## Known limitations

- Instrument music is not played: the emote mod plays its `Sound` records from
  scripts on the player.
- A modded idle form played through a vanilla `Idle*` event is captured as that
  event, so a peer plays its own clip.
- A step within an idle (`IdleBook_PageTurn` within `IdleBook_Read`) becomes
  the held idle, so re-entry plays only that step, or nothing.
- Re-entry plays the entry clip; the `*EnterInstant` wildcards are inactive in
  the default state.
- A paired idle (vampire feeding, Namira's cannibalism) plays alone.
- An idle and an attack within one checkpoint interval keep no order; an attack
  or hand presentation releases a drawn combat idle.
- Idle events beyond 2 a second are dropped at the source.
