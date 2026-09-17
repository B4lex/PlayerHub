# Power Attacks

## Status

Implemented, pending a Windows build and in-game check. The server side is
covered by unit tests. Graph event names, transitions, and conditions come from
the vanilla `Skyrim - Animations.bsa` behavior file `1hm_behavior.hkx`, and the
attack data flags from the `NordRace` record in `Skyrim.esm`. The items under
[Engine assumptions](#engine-assumptions) still need to be checked in game.

## Goal

A remote actor plays the power attack its source player performs, instead of
the plain attack of the same hand (specs 004 and 012):

- **Directional power attacks.** A standing, forward, backward, left, or right
  power attack with a right-hand weapon, a two-handed weapon, or a left-hand
  weapon plays the same direction on peers.
- **Dual power attacks.** A dual-wield power attack, and the power attack of
  both fists, play as such.
- **Unarmed power attacks.** A power punch with either fist plays the forward
  power punch of that hand.
- **Power bashes.** A power bash plays the power bash.

Nothing is hit, staggered, or damaged, and no stamina is spent or replicated.
Only the animation plays.

## Non-goals

- Sprint power attacks as their own animation. A sprinting power attack is
  presented as the forward power attack of the same hand. Sprint attacks are
  started by idle records with perk conditions (`AttackRightForwardSprinting_2hand`
  and its children), which the action stream does not replicate.
- Shield charge (`BlockBashSprint`).
- Stamina, perks, the power attack slow motion, and kill moves.
- Power attacks of creature or werewolf graphs.

## Vanilla attack data

Each race's attack data (`ATKD`/`ATKE`) names the graph event that starts an
attack. The flags that matter are `2` (bash), `4` (power attack), and `8` (left
attack, `BGSAttackData::IsLeftAttack`). The player race's power attack data:

| Hand | Events | Flags |
| --- | --- | --- |
| Right or two-handed | `attackPowerStartInPlace`, `attackPowerStartForward`, `attackPowerStartBackward`, `attackPowerStartLeft`, `attackPowerStartRight` | power |
| Right, sprinting | `attackPowerStart_Sprint`, `attackPowerStart_2HMSprint`, `attackPowerStart_2HWSprint` | power |
| Left | `attackPowerStartInPlaceLeftHand`, `attackPowerStartForwardLeftHand`, `attackPowerStartBackLeftHand`, `attackPowerStartLeftLeftHand`, `attackPowerStartRightLeftHand` | power, left |
| Left, sprinting | `attackPowerStart_SprintLeftHand` | power, left |
| Fists | `attackPowerStartForwardH2HRightHand`, `attackPowerStartForwardH2HLeftHand` (left), `attackPowerStartH2HCombo` | power |
| Both weapons | `attackPowerStartDualWield` | power |
| Bash | `bashPowerStart` | bash, power |

The direction is part of the event name, after which the left-hand events add
`LeftHand`. `attackPowerStartLeft` is therefore a right-hand attack toward the
left, and `attackPowerStartLeftLeftHand` a left-hand one.

## Vanilla behavior graph

`1HM_Behavior`'s ready state takes every power attack event above except the
sprint variants. `IsNPC` is 1 on a remote actor; none of these conditions test
it.

| Event | Condition |
| --- | --- |
| `attackPowerStart{InPlace,Forward,Backward,Left,Right}` | `iRightHandType > 0` |
| `attackPowerStart{InPlace,Forward,Back,Left,Right}LeftHand` | left hand type is a one-handed weapon (1 to 4) or a torch (11) |
| `attackPowerStartForwardH2HRightHand` | `iRightHandType == 0`, `bWantCastLeft == 0` |
| `attackPowerStartForwardH2HLeftHand` | `iLeftHandType == 0`, right hand not a one-handed weapon |
| `attackPowerStartH2HCombo` | both hand types 0 |
| `attackPowerStartDualWield` | both hands weapons other than two-handed, bow, staff, magic, shield, torch |
| `bashPowerStart` | none |

The right and left attack states of `1HM_AttackBehavior` also take the power
attacks of their hand, so a power attack can follow a swing in its combo window,
as on the player. The power attack states themselves take no further attack.
Each power attack state machine (`AttackPowerFwdBehavior` and the others) has an
NPC and a player variant; a remote actor plays the NPC one.

A raised block only takes some power attacks (`IsNPC == 1`: in place, backward,
left, right, the right fist, and the left-hand in place). This does not matter,
because the receiver lowers the block before any attack other than a bash
(spec 012).

## Protocol

### `animation:action`

Two fields are added to the `attack` family:

| Field | Type | Meaning |
| --- | --- | --- |
| `power` | boolean, required | The attack of `action`'s hand, or the bash, is a power attack. |
| `direction` | `standing`, `forward`, `backward`, `left`, `right` | The direction of the power attack. Required when `power` is `true`, and rejected when it is `false`. |

The server and the client reject a missing or non-boolean `power`, a missing or
unknown `direction` on a power attack, and any `direction` on a plain attack.
Actions recorded before this change have no `power` and no longer replay. Graph
event names still never cross the network.

`bash`, `dual`, and unarmed power attacks have no direction. The source sends
`standing` for them and the receiver ignores it.

## How the player starts a power attack

The player does not start a power attack directly. Pressing the attack button
starts the plain swing (`attackStart`), and the attack state edge shows the
plain attack data (observed in game: every edge logged `attackEvent=attackStart`).
When the button is still held after `AttackBlockHandler`'s
`initialPowerAttackDelay` (or `subsequentPowerAttackDelay` in a combo), the
engine sends the power attack event into the playing swing and replaces the
high process's attack data. The attack state stays in the swing, so no new
attack edge marks it. The graph's attack states take the power attack events
for this reason.

The action stream follows the engine: a held attack is a plain attack action
followed by a power attack action of the same hand. Publishing the plain swing
right away, rather than waiting to see whether it becomes a power attack, keeps
plain attacks as responsive as before.

## Source capture

`CombatSync::CaptureLocalAttack` keeps the attack start edges of specs 004 and
012, and classifies each captured attack from the high process's current
`BGSAttackData`:

- At an attack edge the attack is captured as below.
- While the same attack window stays active without a new edge, attack data
  that turns into a power attack is captured once more as a power attack. It
  is not captured again until the next edge, and never during a bash.

The classification:

1. `power` is the attack data's power flag.
2. The hand is chosen as in spec 012. `attackPowerStartH2HCombo` is `dual`.
3. The direction comes from the event name
   (`AnimationEvents::GetPowerAttackEventDirection`): the `LeftHand` or
   `RightHand` suffix is removed, then a name containing `Forward` or `Sprint` is
   `forward`, one containing `Back` is `backward`, and one ending in `Left` or
   `Right` is `left` or `right`. Anything else, including a modded event, is
   `standing`. `dual` and `bash` are always `standing`.
4. A bash found from the `kBash` attack state is a power bash only if the attack
   data is bash data. Data left over from a power swing does not count.

The log lines "observed local attack state edge" and "observed local power
attack of the playing swing" show the chosen action, power, direction, and the
attack data's event.

## Receiver

`ActorAnimation::DispatchAttack` takes the whole attack. Readiness, chaining,
ownership, retries, timeouts, and the locomotion replay are those of spec 004
and 012 for every attack.

A power attack of the same hand as the playing network attack powers that
attack up (`CanPowerUpAttack`), as long as it is not a bash or already a power
attack. It does not wait for the actor's attack state: a remote actor reports it
well into the swing, so waiting showed about 0.7 s of the plain swing before the
power attack, against about 0.25 s on the source (observed in game). A graph
that has not yet entered its attack state rejects the event, and the action is
retried. The power event is sent into
the playing attack, which stays owned; the playback timer restarts and the
attack state is observed again, so the transition cannot end ownership early. A
rejected power-up has the bounded retries of any action and has no plain
fallback, since the plain swing is already playing. Otherwise the power attack
chains in a combo window or starts as a new attack.

When a plain attack and a following power attack of the same hand are both
queued before the plain attack starts, the plain attack is dropped and the power
attack starts directly, so its beginning is not shown twice. A power attack
waiting behind a playing attack logs why once ("power attack waits for the
playing attack").

For a power attack it sends:

| Action | Actor's hands | Event |
| --- | --- | --- |
| `normal` | right hand holds something | `attackPowerStart{InPlace,Forward,Backward,Left,Right}` by direction |
| `normal` | right hand empty | `attackPowerStartForwardH2HRightHand` |
| `left` | left hand holds something | `attackPowerStart{InPlace,Forward,Back,Left,Right}LeftHand` by direction |
| `left` | left hand empty | `attackPowerStartForwardH2HLeftHand` |
| `dual` | either hand holds something | `attackPowerStartDualWield` |
| `dual` | both hands empty | `attackPowerStartH2HCombo` |
| `bash` | any | `bashPowerStart` |

- A new power attack the graph rejects outside a combo window is sent again, in the
  same call, as the plain attack of its hand (spec 012), including that event's
  own dual-wield fallback. This covers graphs that lack the power attack of the
  actor's hands or the direction, and it is logged as `presentedAsPlain`.
- A power attack rejected in a combo window is not sent as the plain attack.
  It waits for the playing attack to finish and then starts as above, like a
  rejected chained swing.
- Every dispatched power attack is logged with its event, fallbacks, and the
  actor's stamina.

### Translation

A forward power attack's lunge is animation motion. The source's movement state
comes from its movement input, so its checkpoints can say `running` at a low
measured speed while the lunge accelerates, and `idle` while the lunge still
carries it forward. The actor followed the last measured speed and fell behind,
then snapped to the target on the `idle` checkpoint (observed in game: a slow
start, then a teleport).

While a network attack owns the graph, `ActorMovement::ApplyTranslation`
therefore never snaps on an idle checkpoint. It moves toward the latest target
at the greater of the measured speed and the remaining distance over 0.2 s, like
an airborne actor. When the attack ends, the ordinary rules apply again.

## Isolation

No new hooks. A power attack or power bash reaches the melee hit frame
(`WeaponSwingHitHook`, spec 008) like a swing and hits nothing.

## Engine assumptions

To verify in game:

1. Observed: the attack edge of a held attack has the plain `attackStart` data.
   Still to verify: `high->attackData` becomes the power attack's data during
   the same attack window, so "observed local power attack of the playing swing"
   is logged with the directional event for every held attack.
2. A held swing chained in a combo window is a new edge with plain data followed
   by the power data, like a first swing.
3. A remote actor accepts every event of the receiver table from the playing
   plain attack of the same hand ("dispatched power attack" with
   `poweredUpPlayingAttack=true`), and from its ready state with the matching
   hands, standing and moving, and plays the NPC variant.
4. A power attack's attack state returns to `kNone` before the 4 s playback
   fallback, so the next action is not held.
5. A remote actor with no stamina still plays power attacks (check `stamina=` in
   the dispatch log).
6. A power bash from a raised block returns to the block like a bash.
7. The forward power attack is an acceptable presentation of a sprinting power
   attack while the actor keeps its network translation.
8. The remote actor's own clip motion does not visibly add to or fight the
   network translation during a lunge.

## Acceptance criteria

1. Standing, forward, backward, left, and right power attacks with a one-handed,
   two-handed, or left-hand weapon play the same direction on peers.
2. Dual-wield, two-fist, single-fist power attacks, and power bashes play their
   power animation on peers.
3. A power attack that the peer's graph cannot play shows the plain attack of
   the same hand.
4. A power attack after a swing plays after or chained into that swing, in the
   source's order.
5. Nothing is hit, staggered, or damaged by a remote power attack.
6. A missing or non-boolean `power`, a power attack without a known
   `direction`, and a plain attack with a `direction` are rejected by the server
   and the client.
