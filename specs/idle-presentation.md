# Idle Presentation

## Scope

A remote actor plays the vanilla idles its source plays: gestures, dances,
sitting cross-legged, leaning, kneeling, meditating, instruments, and any other
`Idle*` graph event the source's graph accepts, from the emote mod "Idle
Animations Wheel Menu SSE" (`IdlePlayWheelMenu.esp`), the console, scripts, or
other mods. Each viewer sends the event to the remote actor's own behavior
graph, so a viewer with an idle replacer sees that replacer. No pose crosses
the wire, and nothing in the viewer's world changes.

This system owns the source capture of idle events, the source's held idle, the
receiver's idle queue, the remote actor's idle ownership, and the release and
re-entry of an idle. The wire fields are in [Network](network.md#actions) and
[Network](network.md#checkpoint-and-context). Furniture idles stay with
[Native interactions](native-interactions.md#furniture), mounts and death with
the same system, and beast forms with
[Combat presentation](combat-presentation.md#beast-actions). The idle shares the
actor's graph with [Locomotion](locomotion.md) and
[Combat presentation](combat-presentation.md), which hold their own graph
writes while an idle owns it ([Ownership](#ownership)).

Idle names, transitions, and clip annotations come from the vanilla
`Skyrim - Animations.bsa` behavior files (`0_master.hkx`, `mt_behavior.hkx`,
`idlebehavior.hkx`, `1hm_behavior.hkx`, first and third person).

## Components

| Module | Responsibility |
| --- | --- |
| `sync/idle/IdleSync` | Source capture, the held idle, and publishing; receiver queue, held-idle state, release decisions, and re-entry |
| `actor/IdlePresentation` | Start gating (`CanStart`), `Dispatch`, `Release`, and the idle's ownership of the graph |
| `actor/GraphStance` | The idle reading of the graph's active states ([Graph reading](#graph-reading)) |
| `runtime/PlayerGraphEventHook` | The local player's accepted graph input events |
| `protocol/IdleEvent.h` | `Multiplayer::IdleEvent` and the name rules both ends apply |

## State and actions

- **Ordered actions** for every idle event the source's graph accepts, so
  entering, exit, and stop events keep their order. An action is the `idle`
  kind of `Action`, anchored to a checkpoint and graph epoch like an attack
  ([Combat presentation](combat-presentation.md#action-stream)).
- **Latest-wins state** for the idle the source is held in: the `Context`
  field `idle` names the event that entered it, empty for none. A peer that
  missed the action (it joined later, its actor respawned or recovered, or the
  action was dropped) re-enters the idle from it ([Re-entry](#re-entry)). A
  one-shot gesture that already ended is never in it.

## Source

### Capture

`PlayerGraphEventHook` passes every event the player's graph accepts to
`IdleSync::ObserveLocalGraphEvent` on whichever thread raised it. That returns
at once unless the player is human (an atomic the main loop sets), keeps a name
that starts with `Idle` in any case or is `OffsetStop`, passes the
[name rules](network.md#actions), and is not `IdleForceDefaultState` or
`IdlePlayer`, and copies it into a fixed queue of 32 under a lock.
`IdleForceDefaultState` is the engine's reset that ends every idle, and the
engine sends `IdlePlayer` at furniture entry, where it returns the idle machine
to its initial state.

Each pass `IdleSync::QueueLocalIdles` drains the queue. While furniture (an
occupied reference), a mount, death, swimming, being in midair, or a beast form
owns the player, every event is dropped and the held idle ends. A real chair
sends the player `IdlePlayer` and `IdleChairFrontEnter` (seen in the graph
event survey); the occupancy is set when the player activates the furniture,
before its entry idle plays ([Native interactions](native-interactions.md#source)),
and lasts until its exit clip ends, so the drain sees it for the whole
interaction. The sit/sleep state does not count: an idle in the unarmed root's
`FurnitureState`, such as `IdleSitCrossLeggedEnter`, sets it to sitting about
2 s in without occupying anything, and its exit clip returns it to Normal
(observed in game).

- An **exit** is an event whose name contains `Exit` or ends in `Stop` or
  `StopInstant` (`IdleChairExitStart`, `IdleRailLeanExit`, `IdleStop`,
  `IdleOffsetStop`, `OffsetStop`). It is queued and ends the held idle only
  while one is held, since the engine and scripts also stop idles peers never
  received.
- Any other event is **entering**: it is queued and becomes the held idle.
- `OffsetStop` is captured beside the idles: the vanilla carry-arm machines
  (`CarryLeftArmBehavior`, `CarryRightArmBehavior`) hold an overlay that some
  idle clips start through an annotation, such as the drink `IdleMQ201Drink`'s
  clip leaves in the right hand (`OffsetCarryMQ201DrinkR`), until `OffsetStop`,
  which the emote mod sends on its exits. It is sent in that spelling.
- At most 2 events a second are queued, with a burst of 4; more are dropped
  and logged (`Idle sync dropped local idle event over the rate limit`). Idle
  actions share the server's action allowance with attacks, and an action the
  server drops leaves a sequence gap that rejects the player's later actions.
- `CheckpointPublisher` samples the held idle with each checkpoint
  (`IdleSync::SampleLocalIdle`), and the queued events are published right
  after it, anchored to it (`IdleSync::PublishLocalIdles`), so the context that
  holds an idle and the action that entered it name the same checkpoint. A
  graph epoch change, a disconnect, or leaving the world discards queued
  events. A furniture or beast-form checkpoint publishes none.

### Held idle

The held idle ends, and the next context clears it, when:

- an exit event is captured, or the drain finds the player blocked (above);
- the sampled checkpoint has furniture, a mount, death, swimming, or a beast
  form, or its state changed from the previous sample by a start of movement,
  a jump, a draw or sheathe, or a sneak toggle. A stop does not end it, since
  the source's published stop can trail the idle's start
  ([Locomotion](locomotion.md#source-capture));
- the [graph reading](#graph-reading) of the player, every 0.1 s, is back in
  the default state after it was seen in an idle, or never saw one within 0.5 s
  of the event. The idle machine switches state on the graph update after the
  event. A one-shot gesture ends this way through its clip's `IdleStop`
  annotation (`IdleLaugh`, `IdleSalute`).

A reading without any vanilla idle machine keeps the idle until an exit or a
state change. Logs: `Idle sync holding local idle`, `Idle sync ended local
held idle` with the reason, `Idle sync captured local idle event`, and
`Idle sync ignored local idle event` with the reason.

## Receiver

### Queue

`IdleSync::AcceptRemoteAction` accepts an idle action with a known checkpoint,
an epoch no older than it, and no furniture in it, into a bounded FIFO (32),
interning the event name as a `BSFixedString` once. Queued actions time out
after 3 s; a graph epoch boundary drops the old epoch's; retirement, a lost
actor, mounting, and leaving the local space clear the queue, but not the held
idle.

`IdleSync::SyncIdle` runs each pass for a human actor on foot outside
furniture, before the attack and hand presentation stages. The front action
waits for its epoch and anchor checkpoint to play, then for a start the
remote actor can take (`IdlePresentation::CanStart`, the checkpoint, and the
equipment stage):

- the actor is ready, its locomotion revision settled, and no recovery,
  locomotion reassert, release settle, attack, hand presentation, or unsettled
  weapon transition owns the graph, and the graph is the one the dispatch
  tracks;
- neither target nor checkpoint jumps or swims, and the actor's `WEAPON_STATE`
  is fully drawn or fully sheathed as the checkpoint says. The emote mod sends
  `unequip` before most emotes, so the checkpoint's sheathe plays to its end
  first ([Combat presentation](combat-presentation.md#weapon-stance));
- the hands have reconciled the latest equipment revision, no attack is
  queued, and the checkpoint holds no block, bow draw, or cast.

`IdlePresentation::Dispatch` sends the event through the firewall bypass under
`GameThread::Require`, one per pass. A rejected event is sent again at most 4
times, 0.25 s apart, then dropped with `Idle sync remote graph rejected idle
event` (the name, throttled per peer): a viewer whose graph lacks a modded
event shows nothing. An exit that arrives while no idle plays on the actor is
dropped. Accepted events log `Idle sync processed idle action`.

### Ownership

An accepted entering event makes the idle own the graph
(`IdlePresentation::IsPlaying`). `IdlePresentation::Advance`, at the start of
each locomotion update, reads the actor's [graph](#graph-reading) every 0.1 s
and ends ownership once the graph is back in its default state after it was
seen in an idle, or never saw one within 0.5 s (`Idle sync remote idle ended`).
An accepted exit keeps ownership until its exit clip returns the graph, so
nothing cuts it short. A graph without the vanilla idle machines keeps
ownership until a release.

While an idle owns the graph, nothing the plugin does on its own ends it:

- stance reconciliation and locomotion reasserts wait
  ([Locomotion](locomotion.md#stance-reconciliation)), and furniture entry and
  idle starts wait for a release;
- the sitting state such an idle sets on the actor is not unwound as a seat
  without occupancy ([Native interactions](native-interactions.md#exit)); once
  ownership ends, a sit/sleep state the idle left is;
- the weapon transition's `IdleForceDefaultState` never runs during an idle,
  because an idle starts only once a transition settled and a draw or sheathe
  releases it first;
- the per-pass locomotion variables, the idle turn hold, and its `turnStop`
  stay: no vanilla idle state reads those variables or has a `turnStop`
  transition, which belong to the default states the idle replaced;
- the actor's own AI, packages, and scenes start or end no idle: the firewall
  rejects every event they send ([Remote actor](remote-actor.md#animation-firewall)),
  and the package takeover and scene suppression send none past it.

### Release

A change the source's idle does not outlast releases an owned idle:
`IdleSync::OnCheckpointPlayed` notes a start of movement, a jump, a draw or
sheathe, a sneak toggle, a start of swimming, or furniture in the played
checkpoints, and `SyncIdle` acts on it, as on furniture, a queued attack, or a
held block, bow draw, or cast in the checkpoint. `IdlePresentation::Release`
sends `IdleForceDefaultState` past the firewall
(`AnimationFirewall::ForceDefaultGraphState`), which restarts the unarmed root
state (`MT_RootBehavior`'s self-transition wildcard) and so ends every unarmed
idle, and gives the graph one update before the next locomotion dispatch. An
entering idle anchored at or before the change is dropped, since the source
already left it. Death, mounts, beast forms, and respawns replace the actor's
graph or its update path.

When the held instance the actor was given ends without an exit the actor
accepted, and none is queued, the idle is released 1 s later (`held-ended`).
The source's graph left it by itself or through events peers never receive,
or the exit timed out in the queue. The 1 s is longer than the actor's idle
trails its source's, so a gesture that ends on both graphs is not cut short.
Logs: `Idle sync released remote idle` with the reason.

### Re-entry

`OnCheckpointPlayed` follows the played checkpoints' `idle`. A change starts a
new instance of the held idle, named by the sequence of the first checkpoint
that showed it. The actor's graph records the instance it was given the
entering event of (`IdlePresentation::SetAppliedInstance`), and graph
invalidation clears the record with the rest of the dispatch state.

- An accepted or finally rejected queued action whose event is the held idle
  records the current instance. The source publishes the action anchored to
  the checkpoint whose context first holds the idle, so the two match.
- With no queued action, a held instance the graph was not given is sent once
  it has been held 0.3 s, under the same start rules and retries
  (`Idle sync re-entered held idle from state`). That covers a late joiner, a
  respawn, graph replacement, hit recovery (after its checkpoint restore), and
  a dropped action, and never replays a gesture that ended, which the held
  idle no longer names. The idle plays from its start.

## Graph reading

`GraphStance::ReadStanceSelectors` reports an `idle` axis from the vanilla
machines that the `Idle*` events enter. It reads on while any of them is in an
idle state, off while all active ones are in their default state, and none
when none is active. It is never mixed: the root machines stay in their default
state while a nested machine plays an idle.

| Machine | Default | Idle states |
| --- | --- | --- |
| `MT_RootBehavior` (unarmed root) | `MT_State` | every other state: furniture, sitting, kneeling, meditating, leaning (`FurnitureState`), instruments, salutes, dances (`NonOffsetIdles`), chairs, beds, `IdleStopIdles`, ... |
| `MT_BehaviorGraph` | `MT_Default_State` | `InteractionObjectState` (gestures: `IdleLaugh`, `IdleExamine`, `IdlePointFar_02`, `IdleWave`, ...), `MT_BoyRitualState` |
| `MT_Idle_3rdPersonBehavior` (dialogue idles) | `InitialIdleState` | every other state |
| `CarryLeftArmBehavior`, `CarryRightArmBehavior` | `MTLeftArmState`, `MTRightArmState` | every other state (carry-arm overlays) |
| `1HM_Behavior` | `1HM_Ready_State` | `CombatIdles` |
| `RootBehaviorGraph` | `Default_State` | `FullBodyPlayerIdles`, `StandingVampireFeed` |
| `Master_Behavior` | `Root` | `IdleDLC2TentacleWarpBook` |

A gesture returns to the default state through its clip's `IdleStop`
annotation, a held idle through its exit clip's `IdleFurnitureExit` (for
example `IdleSitCrossLeggedEnter` loops in `IdleSit_CrossLegged` until
`IdleChairExitStart`). None of these names occur in the werewolf or vampire
lord graphs.

## Engine assumptions

Not confirmed in game:

1. A non-player actor's graph accepts the vanilla idle events the player's
   graph accepts and plays the same idle: the transitions above carry no
   `IsNPC` condition, and both use the shared character behaviors.
2. Instrument and prop idles (lute, flute, drum, horn, broom, ledger, drink
   tray) attach their props from the animation itself (`AnimObjDraw` and
   related clip annotations, which reach the actor as graph output events), so
   a remote actor shows them without further work.
3. A state machine's current state switches on the graph update after an
   accepted event, and `IdleForceDefaultState` returns every unarmed idle,
   including furniture-style idles, to the default state.
4. The engine and scripts do not send the player an entering `Idle*` event
   outside furniture other than for an idle it plays, so nothing else is
   relayed as an idle.

## Acceptance criteria

1. A gesture plays once on peers, through each peer's own graph, including a
   peer's replacer.
2. Sitting cross-legged, leaning, kneeling, meditating, playing an instrument
   (with its prop), and dancing hold on peers until the source exits, and the
   exit clip plays on peers.
3. A peer that joins, or whose actor respawns or recovers from a hit, while the
   source is held in an idle sees the source in it; an ended gesture is not
   replayed.
4. An emote started with the weapon drawn plays the sheathe first, then the
   idle.
5. Moving, jumping, drawing, sneaking, or sitting in furniture out of an idle
   ends it on peers, and the actor walks without sliding in the idle pose.
6. Real chair furniture plays only the furniture's own idle on peers, with no
   duplicate.
7. An idle event a peer's graph lacks shows nothing there and logs
   `Idle sync remote graph rejected idle event` with its name.

## Known limitations

- Instrument music is not played: the emote mod plays its own `Sound` records
  from its scripts on the player.
- Modded idle events play only on peers whose graph has them; there is no pose
  fallback yet. A modded idle in a state outside the vanilla idle machines is
  held only for the 0.5 s entry window, so a late joiner does not see it.
- An event that steps within an idle (`IdleBook_PageTurn` within
  `IdleBook_Read`) becomes the held idle, so a peer re-entering from state
  plays only that step, or nothing where its graph takes the step only inside
  the idle.
- A re-entered idle plays its entry clip; the `*EnterInstant` variants are
  wildcards of the idle's own machine, inactive in the default state, so they
  cannot skip it.
- A paired idle (vampire feeding, Namira's cannibalism) plays alone, as beast
  feeding does.
- An idle and an attack captured within one checkpoint interval keep no order
  between them. An attack or a hand presentation releases a weapon-drawn combat
  idle.
- Idle events beyond 2 a second are dropped at the source.
