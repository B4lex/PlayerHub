# Remote Actor

## Status

Implemented for Skyrim AE 1.7.104, and run in game there.

- **Spawn, readiness, animation firewall, hit recovery:** run in game with
  every synchronized feature.
- **Registry shared lock and identity indexes:** Linux syntax check only; not
  built on Windows or run in game.
- **Combat isolation:** hook sites and entry bytes checked against the 1.7.104
  executable and address library; behavior not yet verified in game.
- **Precision exclusion:** interface layout and disable behavior read from
  Precision's source (`PrecisionAPI.h`, `PrecisionHandler`, `Hooks.cpp`); Linux
  syntax check only. Not built on Windows or run with Precision installed.
- **Reaction suppression and package takeover:** run in game on 2026-09-20:
  every hit met a blocked dialogue response and one package takeover, and the
  repeating weapon equip animation after a hit was gone. Sitting in furniture
  after a hit is not covered by the captures yet.
- **Scene package suppression:** traced in the 1.7.104 binary; not verified in
  game.
- **Graph recovery on SE (2026-10-12):** `RevertAnimationGraphManager`'s
  SE ID was 32140, a thunk to a different graph routine (32192); it is now
  32139, which calls the same graph functions as 1.7.104 ID 32883 in the
  1.5.97 disassembly. Linux syntax check only; not run on 1.5.97.
- **World isolation:** placement verified in game with a replayed player
  walking, idling, mounting, riding, and dismounting. Its other acceptance
  criteria are not yet checked.

On runtimes other than 1.7.104 the entry detours are not installed and a
warning is logged.

## Scope

A remote actor is a local `RE::Actor` that stands in for a player who lives in
another world. This system owns what makes it one: spawning and registration,
readiness, the per-actor engine update, the animation firewall, graph recovery,
survival, and isolation from the local world in both directions. The local
world neither fights, moves, nor is moved by a remote actor, and the actor's
own AI does not react to the local world. Local gameplay continues as if it
were not there.

What the actor presents comes from the feature systems:
[Locomotion](locomotion.md), [Combat presentation](combat-presentation.md),
[Appearance and equipment](appearance-and-equipment.md), and
[Native interactions](native-interactions.md). When it is spawned and retired
is decided by the [sync pipeline](sync-pipeline.md#actor-lifecycle).

## Components

| Module | Responsibility |
| --- | --- |
| `actor/ActorSpawner` | Create, remove, and save-exclude a remote actor reference |
| `actor/ActorRuntime` | Readiness and the fixed per-actor engine update, for actors, riders, and mounts |
| `actor/RemoteActor.h` | The replicated instance: handle, readiness, and `ActorMovement`, `ActorAnimation`, `ActorFurniture` state |
| `actor/ActorRegistry` | Registered identity and per-actor flags, queried under a lock by hooks on any thread |
| `actor/AnimationFirewall` | Rejects locally injected graph events; network dispatch bypasses it |
| `actor/SurvivalPolicy` | Actor flags, combat sanitization, package takeover, damage, kill, and magic guards |
| `actor/CombatIsolation` | Hostility, combat start, and hit hooks |
| `actor/PrecisionIsolation` | Excludes remote actors from Precision through its interface |
| `actor/ReactionSuppression` | Dialogue, flee, and detection hooks |
| `actor/ScenePackageSuppression` | Scene package action hooks |
| `actor/RemoteCollision` | Placement ownership, the skipped movement update, collision, support, and Speed channel |
| `runtime/CodeDetour` | Shared entry detour: copies checked entry bytes, at the entry or past an entry guard, into a trampoline gateway |

## Spawn

`ActorSpawner::Spawn` creates the reference with `CreateReferenceAtLocation`
directly at the target position and heading, in the loaded exterior cell that
contains it or the local player's interior, as a persistent reference so it
survives ordinary exterior cell unloads within one worldspace.

- The base is the runtime `TESNPC` that
  [Appearance and equipment](appearance-and-equipment.md#actor-base) builds.
  `PlayerBase` (`00000007`) is refused: `CreateReferenceAtLocation` returns a
  reference for it that never completes actor and 3D initialization.
- Activation is blocked, so Skyrim starts no dialogue or other interaction
  with it. Activating it still sends `TESActivateEvent`, which opens its
  player's menu ([UI overlay](ui-overlay.md#player-menu-in-the-world)).
- AI stays enabled for the actor's whole life. The graph and character
  controller only finish initializing and keep updating while the actor stays
  in Skyrim's normal processing; disabling it clears `kProcessMe`, zeroes the
  controller velocity, and can stop 3D, controller, or graph initialization.
- It is registered with `ActorRegistry` right after creation, before any hook
  can see it, and unregistered before the reference is deleted. Its
  [Precision exclusion](#precision) follows the same span.
- The reference must not carry `kCollisionsDisabled`, the flag `TCL` toggles.
- The update loop never calls `MoveTo(player)`: an exterior actor may occupy
  another parent cell than the local player.

An actor is ready once its 3D, character controller, and animation graph
manager exist. `AssertDoNothingPackage` then installs the do-nothing package as
its run-once package ([Package takeover](#package-takeover)).

## Update order

`ActorRuntime::Update` runs once per pass for an on-foot actor, in this order:

1. validate the handle (a lost one is reported to the lifecycle);
2. advance a playing network attack;
3. `ApplyRemoteReplicaPolicy`, then `SanitizeRemoteCombatState`;
4. recover the graph if a recovery is pending;
5. wait for 3D, controller, and graph (readiness);
6. enable the animation firewall once 3D is loaded;
7. advance an active native furniture interaction, which then owns placement,
   or otherwise place the actor by the network
   ([Placement ownership](#placement-ownership));
8. hold the idle turn state, and apply the staged graph state;
9. gate translation on settlement, except around a landing;
10. attempt a deferred furniture entry;
11. apply heading and translation (`ActorMovement::ApplyTranslation`).

Furniture owns position, heading, and graph while active. A rider and a mount
have their own update (`UpdateRider`, `UpdateMount`,
[Native interactions](native-interactions.md#mounts)).

## Registry

`ActorRegistry` is the only remote-actor state that hooks on worker threads
read. Each operation keeps one lock scope; pure event-name classification in
the graph input hook runs outside the lock. Hooks on hot engine paths skip the
lock when an atomic count says no actor needs them (riders' gait speed,
network-placed actors, native furniture exits).

The lock is a `std::shared_mutex`. Registrations are inserted and erased only
on the main-loop thread (`Register` before the first hook can observe a new
actor, `Unregister` before its reference is deleted), so that thread reads
registrations and the fields only it writes without the lock
(`LockUnlessMainThread`, `LockSharedUnlessMainThread`), and takes it
exclusively to write a field other threads read (`LockForWrite`). Other
threads take it shared to read fields only the main loop writes or to raise an
atomic count (graph input and channel hooks, `IsRegistered`, `IsNetworkPlaced`),
and exclusively to change anything else. Counts that hooks raise from other
threads are atomics the main loop consumes without the lock; observation flags
and hook counts are only accessed under the exclusive lock. Per actor it holds:

- identity and its graph holder, indexed by actor, by handle (so AI threads
  never resolve a handle), and by the actor's `MagicTarget` and
  `ActorValueOwner`, so the damage and magic hooks, which see every actor in
  the world, find a remote actor without scanning;
- ownership flags that open the firewall: furniture, cast, mounted rider, dead;
- `networkPlaced`, the published graph Speed, and mount gait speed;
- the furniture each actor holds, for the local activation hook
  ([Native interactions](native-interactions.md#shared-furniture)).

## Animation firewall

A hook on `Character`'s `IAnimationGraphManagerHolder::NotifyAnimationGraph`
rejects every graph event that local gameplay, AI, or mods send to a registered
actor, and logs rejected events per event name, throttled. Network presentation
is sent directly to the saved graph function, bypassing the hook.

- The firewall is enabled after the graph accepts its initial network state, so
  graph construction is not filtered. It opens while a replacement graph
  initializes and closes again once the network state is restored.
- Some native transitions are admitted because Skyrim must run them:

| Admitted | Why | Owner |
| --- | --- | --- |
| `WeapEquip`, `WeapUnequip`, `Equip`, `Unequip` | Native draw and sheathe | [Combat presentation](combat-presentation.md#weapon-stance) |
| `Magic_Equip`, `MagicForceEquip`, `MagicWeap_ForceEquip`, `MagicForceEquipRight`, `MagicForceEquipLeft`, `WeapSoloEquip`, `Magic_Solo_Equip` | Spell hand draw and single-hand swaps | [Combat presentation](combat-presentation.md#spells) |
| `MLh_*`, `MRh_*`, `DualMagic_*`, names containing `Cast` (case-insensitive) | While a cast is owned | [Combat presentation](combat-presentation.md#spells) |
| `SwimStart`, `swimStop` | Skyrim manages swimming locally | [Locomotion](locomotion.md#swimming) |
| every event | While furniture, a mount, or death owns the actor | [Native interactions](native-interactions.md) |

- On a mount, the gait events Skyrim sends are blocked instead
  ([Native interactions](native-interactions.md#mounts)).
- Local attack, hit, cast, combat-target, and AI-driven events never take
  graph ownership outside those windows.
- `ForceDefaultGraphState` sends `IdleForceDefaultState` past the filter, then
  raises a carried torch again, because the reset also drops the graph's torch
  arm layer.

### Graph channels

Skyrim copies each actor's own movement into graph variables during every
animation update, after main-loop writes. For a registered actor the hooks
report the network values instead: `ActorDirectionChannel` reports the network
direction, `ActorTurnDeltaChannel` reports 0, and `ActorSpeedChannel` reports
the graph Speed `ActorAnimation` published (a mount or rider reports its gait
speed). See [Graph speed](#graph-speed).

### Graph recovery

Some hit reactions enter the graph below `NotifyAnimationGraph`, so filtering
alone is not enough. A `TESHitEvent` for a registered actor, and stagger or
recoil state seen by polling, queue a recovery; the hit callback never touches
the actor, since it can overlap animation work. On the next update the
firewall opens and Skyrim's native `RevertAnimationGraphManager` (ID 32883,
SE 32139) resets the
whole graph, including variables and partly active transitions. `DoReset3D(false)`
is the fallback when the revert fails or the graph is not ready within 2 s.
Translation stays paused until graph and controller have been ready for two
consecutive passes; then all dispatch bookkeeping is invalidated and the newest
locomotion and presentation checkpoint is replayed. A detected graph manager
replacement also resets the bookkeeping.

## Survival and AI policy

- `ApplyRemoteReplicaPolicy`, on every update, clears `kPlayerTeammate`,
  `kCanDoFavor`, `kCanSpeak`, `kAttackOnSight`, and `kAngryWithPlayer`, and
  disables attacking and casting. Presentation clears `kAttackingDisabled` or
  `kCastingDisabled` only for one synchronous dispatch, and a held cast keeps
  `kCastingDisabled` clear.
- Immediately before Skyrim's actor update, the live AI process is marked as
  ignoring combat and any combat controller is marked inactive. The original
  update still runs, so graph and controller processing continue.
- `SanitizeRemoteCombatState`, once per pass, stops combat, clears flee flags,
  stops alarms, resets a fear emotion, and takes back any engine-selected
  package ([Package takeover](#package-takeover)).
- Guards on `KillImpl`, `KillDying`, `HandleHealthDamage`, and health
  `ModActorValue` keep the actor alive, and a magic firewall keeps local magic
  off it. A dead registration lets these calls through and stops reasserting
  the policy ([Native interactions](native-interactions.md#death)).

## Combat isolation

Local enemies, creatures, guards, and followers never become hostile to a
remote actor, target it, or start combat with it, and its replicated attacks
hit nothing.

### Why enemies attacked remote actors

The actor used to carry `kPlayerTeammate`. In 1.7.104 the hostility decision
(ID 37534, `this` the deciding actor) replaces a teammate target with the
player itself, so every actor hostile to the local player was hostile to every
remote actor, and its wrapper ID 37532, used by `CombatGroup` target handling,
returns true for a teammate without consulting the decision. Clearing the flag
alone is not enough: very aggressive actors attack neutral targets.

### Hooks

Three functions are hooked at their entry, so every caller sees the same answer,
including AI worker threads:

| ID | Function | Moved entry bytes | A remote actor on either side |
| --- | --- | --- | --- |
| 37534 | Hostility decision `(actor, target, aggression, mode)` | `mov [rsp+20h], r9b` | returns false |
| 37537 | `Actor::IsHostileToActor` (its own faction-reaction path) | `mov [rsp+10h], rbx` | returns false |
| 38561 | `Actor::StartCombat` (also reached from hits, without a hostility check) | `mov r11, rsp; push rbp; push rsi` | returns false |

The wrappers 37532 and 37533 (34 callers, including `StartCombat`) both
tail-call 37534. The frenzy check is inside 37534, so frenzied local actors also
ignore remote actors.

Remote attack animations play natively, so their `HitFrame` annotations still
run the local hit pipeline. Three more hooks drop hits:

| ID | Function | Hook | Drops when |
| --- | --- | --- | --- |
| 38603 | Melee hit frame `(attacker, leftHand, flag)`: reach sweep, the swing's target (38628), then the actor hit (38627) at +0x40A for each swept actor and +0x45A for the target | entry detour, `mov rax, rsp; mov [rax+10h], dl` | the attacker is remote; returns without sweeping |
| 38628 | Swing hit target `(attacker)`, returns the target or null (Precision's `CalculateCurrentHitTargetForWeaponSwing`) | entry detour, `mov rax, rsp; mov [rax+8], rcx` | the attacker is remote; returns null |
| 38627 | Actor hit `(aggressor, target, projectile, leftHand)`: HitData, damage, stagger, hit effects, `TESHitEvent` | detour at +9, `mov rax, rsp; mov [rax+8], rcx` | either side is remote; returns |

The melee hook also covers the queued path: when 38603 defers itself (ID
36915), the deferred task (ID 36991) calls 38603 again. Skipping it drops
damage, stagger, hit impulses, hit effects, and the target's `TESHitEvent`.
Bashes and power attacks reach the same hit frame (`WeaponSwingHitHook`).

The actor hit is filtered at its entry rather than at its call sites because
combat mods call it directly: Precision's weapon collisions call 38628 and then
38627 without passing 38603, so a remote actor's swing hit local actors. Its
callers in the executable are 38603 and the projectile impact (ID 44218 +
0x90), so a local arrow or bolt that strikes a remote actor is dropped as well,
not only melee. 38627 opens with `test rdx, rdx; je` to its `ret`; the detour
sits after that guard, where the stack and argument registers are still as at
entry, because the relative `je` cannot be moved into a gateway.

### Precision

Precision replaces melee hit detection with weapon collision bodies and applies
its own hit stop, impulses, and hit reactions (`Precision_Hit*` graph events)
before it calls the actor hit. `PrecisionIsolation` takes remote actors out of
it entirely, so they fall back to the vanilla hit frame, which the hooks above
drop:

- `Initialize` at `kDataLoaded` calls `Precision.dll`'s `RequestPluginAPI`
  with `InterfaceVersion::V3` and keeps `ToggleDisableActor` (virtual slot 23
  of the returned interface). Without the module nothing else runs; a module
  without V3 logs a warning, and the actor hit hook is then the only guard.
- `ActorSpawner::Spawn` excludes each new remote actor and mount
  (`ToggleDisableActor(handle, true)`), and `ActorSpawner::Remove` releases it.
  A disabled actor is never added to Precision's world, so it gets no attack
  collisions or ragdoll there, and Precision's swings pass through it.
- Precision clears its disabled actors when the local player's `bhkWorld`
  changes, then waits `fWorldChangedWaitTime` (0.4 s by default) before adding
  actors again. `PrecisionIsolation::Update` compares the player cell's world
  each pass and reasserts every exclusion on each pass for 1 s after a change,
  so the order of Precision's job and the main loop does not matter.

### Detours

`CodeDetour` copies the first entry bytes, which end at an instruction
boundary and are position independent, into an SKSE trampoline gateway that
jumps back to the next byte, then writes a 5-byte branch to the thunk. An
offset moves the detour past an entry guard that only returns. The
bytes are compared first: if another plugin already patched the function, that
hook is skipped and an error is logged. An entry detour pads moved bytes past
the fifth with NOPs. A call-site detour chains to whatever the call pointed to, so
another plugin's hook there keeps working. Startup logs which hooks were
installed. Suppressions are logged with the path, both form IDs, and a count,
throttled.

## Reaction suppression

A remote actor keeps full actor processing, so its AI still evaluates the local
world. Flags reasserted from the main loop remove a reaction only after the
engine chose it, and some paths ignore them: a hit remote actor still spoke hit
lines with `kCanSpeak` cleared. Three functions are therefore hooked where the
reaction is decided. The approach and two IDs come from TiltedEvolution
(`Code/client/Games/Skyrim/Actor.cpp`); AE Address Library IDs are stable, and
the 1.6.640 and 1.7.104 layouts were compared.

| ID | Function | Moved entry bytes | Registered remote actor |
| --- | --- | --- | --- |
| 39643 | AI process dialogue response `(process, DialogueItem by value, speaker, listener, flag)` | `mov rax, rsp; mov [rax+10h], rdx` | as speaker: returns without starting it and releases the item |
| 37577 | `Actor::IsFleeing` | `push rbx; sub rsp, 20h` | returns false |
| 42704 | `ActorKnowledge::SetDetectionState(state)` | `push rbx; sub rsp, 20h` | as knowledge owner: returns without storing or notifying |

- 39643 takes a fifth byte argument on the stack that TiltedEvolution's
  signature omits; the thunk forwards all five. The item is a by-value copy the
  caller (ID 39642) took a reference for, and 39643 releases it. The thunk
  releases it the same way when it blocks, through the engine destructor (ID
  35221) and sized delete (ID 109588); CommonLib's defaulted destructor would not
  free the responses, and TiltedEvolution's hook leaks the item.
- `IsFleeing` has 22 callers. The thunk calls the original and logs only calls
  that would have returned true.
- 42704 stores the state at `ActorKnowledge+0x20`, stamps
  `detectionQueuedTimeStamp`, and notifies listeners. It takes its own reference
  to the state, so skipping it releases nothing. The owner is compared by handle
  (`ActorRegistry::IsRegistered(ActorHandle)`). TiltedEvolution blocks only a
  remote player's detection of the local player; here the remote actor detects
  no one.
- The dialogue log names speaker, listener, topic, and topic info, so a bark
  that still plays can be traced.

These hooks sit apart from `CombatIsolation` because they act on the remote
actor's own AI. `ApplyRemoteReplicaPolicy` and the combat sanitization stay: each
flag they clear also guards paths the hooks do not touch, so dropping one is its
own change with its own in-game test.

### Package takeover

A hit makes the engine re-evaluate packages and select `DefaultMasterPackage`
(`0003EAB9`, type 18). It wants a sheathed weapon while the network holds it
drawn, so the two fought once per update: in the 2026-09-19 capture two hit
actors cycled between `kWantToSheathe` and `kDrawing` for over 600 frames,
while an unhit one stayed on the do-nothing package (27).

`SanitizeRemoteCombatState` therefore restores the do-nothing package whenever
the actor runs any other package, once the actor is ready (`a_mayResetPackage`),
including during furniture interactions: furniture runs on the do-nothing
package, and installing it does not reach `ClearFurniture` (ID 39798) through
`PutCreatedPackage` or `SetRunOncePackage` within four call levels, and
`PutCreatedPackage` skips its sit-state handling for the do-nothing type. Each
takeover logs `Took back local actor package` with the package type and form,
since it lasts a frame or two and the sanitization log's interval would hide
it; the sanitization log also reports it as `unownedPackage`.

Taking back a package the engine reselects every pass never converges. A remote
actor in a tavern was filled into the bard audience quest alias and handed
`BardAudienceTravelToLoc2` (`00108EDD`) and
`BardAudienceStayInLocationAndApplaud` (`00108ED6`): one capture took the same
package back 13,800 times in four minutes, and each stole furniture occupancy
one pass after entry. Two changes contain it:

- `Actor::CheckForCurrentAliasPackage` (vfunc `0x49` on every runtime) returns
  null for a registered actor, so an alias package is refused where the engine
  asks for it. A remote actor is never a participant in a local quest.
- `AssertDoNothingPackage` installs the do-nothing package as the run-once
  package, which outranks quest and scene packages and survives reselection.
  Every path that hands the actor back to synchronization uses it instead of
  `InitiateDoNothingPackage`.

In the 2026-09-20 capture, each of 21 hits across three actors was followed on
the next pass by exactly one takeover of `DefaultMasterPackage`, and no actor
reported `kWantToSheathe` while the network held the weapon drawn.

### Scene packages

The bard audience packages also arrive through a song's scene, not package
evaluation: in a 2026-09-21 capture `BardAudienceTravelToLoc2` was taken back
over 13,000 times in two minutes with no alias package refused, and every
furniture entry in that window lost its occupancy. On 1.7.104 a scene's package
action installs its package on the alias's actor through ID 39149:

- `BGSSceneActionPackage` vfunc `0x10` (ID 24244) starts the action, resolving
  the alias's actor through the scene's quest (ID 25066). When the alias has no
  actor, it marks the action started and ends it through vfunc `0x11` (ID
  24245).
- ID 24252 reinstalls the package whenever the actor runs another one; scene
  updates call it at ID 24130 + `0x9A` and ID 24173 + `0x61`.
- ID 24253 installs packages the same way but has no reference in the binary
  and is not hooked.

`ScenePackageSuppression` refuses both for a registered actor
(`Refused local scene package`, with scene, quest, alias, and package). The start hook resolves
the alias through `TESQuest::GetAliasedRef` and treats a remote actor like an
alias without an actor.

## World isolation

The network transform is the only thing that places a remote actor. Before this,
each actor update integrated its character controller from wherever the network
had put it: root motion was added, the controller pushed out of anything it
overlapped and pushed local bodies, and the next pass moved it back, so an actor
touching the local world shook.

### Placement ownership

`ActorRuntime` decides on every update who places the actor, and
`RemoteCollision` publishes it to the registry (`networkPlaced`):

- an on-foot actor is network-placed once ready, except while native furniture
  owns it;
- a mount is network-placed once ready;
- a rider is always left to Skyrim, which carries it on its mount;
- a dead actor is left to Skyrim's death and ragdoll; `ActorDeath::Kill`
  releases it (`owner=death`) before the kill, and the registry's `dead`
  overrides `networkPlaced`.

`Remote actor placement owner changed` logs each change.

### Skipped movement update

The hook on the movement update call (ID 37350 + 0xFB, the only call of ID
37356) returns without calling it for a network-placed actor. For any other
remote actor it passes through: an actor Skyrim moves passes through the
character body it bumped for that update. ID 37356:

1. reads root motion and the movement controller's output, passes them through
   `ModifyMovementData` (vfunc 0x11A), and stores the process movement speed
   (`high + 0x2A8`, ID 39873);
2. applies the heading change (`SetHeading`, ID 37230);
3. calls `Actor::Move(delta, translation)` (vfunc 0xC8, ID 37507), which
   integrates the controller, or with TCL (form flag bit 4) adds the translation
   through `SetPosition`;
4. unless the actor flies, compares the requested motion with the position
   change and feeds it into the graph (vfuncs 0x11D/0x11E).

Skipping only `Move` would leave step 4 measuring zero motion; skipping the
whole update removes the second writer of position and heading.

### Frame order

The main-loop hook wraps the call at ID 36564 + 0x6E (ID 40438), which reaches
the per-frame actor movement job list (ID 41306, job callback ID 41444, calling
ID 37350). Skyrim's movement update therefore runs before the pass, and the
network transform is the last write before render. The movement update runs on
job threads, so `IsNetworkPlaced` takes the registry lock shared, skipped by an
atomic count when no actor is network-placed.

### Graph speed

The graph's Speed channel (ID 38330 → ID 42767) reads `Actor::GetMovementSpeed`
(ID 37957 → ID 39872), the value step 1 stores, which a network-placed actor no
longer updates. `ActorAnimation` publishes the Speed it writes to the registry
when it changes, and the channel hook reports it for a living network-placed
actor.

### Swimming

`Character::Update` (vfunc 0xAD, ID 37348) decides swimming from the submerged
fraction `(water height − reference Z) / height` (ID 37448), which follows the
network position. At 0.7 or deeper it starts swimming (ID 37946) and requests
controller state 5. Below 0.7 while swimming, it stops only when the
controller's `kSupport` flag (`controller + 0x218`, bit 8) is set or its current
state (`context + 0x20`) is in-air; below 0.01 it requests in-air. These are
requests written to `wantState` (`controller + 0x21C`), applied only inside
`Move`, so with `Move` skipped an actor that started swimming kept swimming on
land.

`RemoteCollision::PlaceByNetwork` sets `kSupport` whenever the checkpoint's
`swimming` is false and clears it while true. The engine's own exit then ends
swimming once the actor is shallower than 0.7 and its source stopped swimming.
An actor deeper than 0.7 in local water keeps swimming even if its source does
not. `Remote actor controller support changed` logs each change.

### Character body

Skyrim gives the player a proxy controller and every other actor a rigid-body
controller (ID 37171 builds `bhkCharProxyController` only for the player or
when a Havok setting or race flag asks). A rigid-body controller's character
body (`GetRigidBody`, ID 79189, `+0x350`) is its world collision: physics
simulates it whether or not the movement update runs, so it collides with,
pushes, is pushed by local bodies, and triggers local volumes.

`RemoteCollision` sets Havok's no-collision filter bit (14) on that body while
the network or a mount places the actor, and clears it whenever Skyrim moves the
actor, so a seated or dying actor still stands on the world. Each pass
reasserts it, since a rebuilt controller collides again. The filter core (ID
78553), which the pair, shape, and ray filters call, rejects any pair where
either side has the bit, unless the first side's layer is 0x2C. The bit is set
without `updateCollisionFilterOnEntity`, so it applies to new contacts and
queries. `Remote actor character body collision disabled` and `restored` log
each change with owner and controller type.

### Body write-back

Every physics step, the rigid-body controller's post-simulation callback (ID
79185, second vtable) reads the controller position (`GetPositionImpl` with the
center offset, ID 79194) and sends it through the move-finish event source
(`controller + 0x10`, ID 38322) to the actor's sink (`actor + 0xD8`), which
places the actor there, unless the controller's `kNoSim` (bit 17 of
`controller + 0x218`) is set. Only the skipped movement update kept that body
upright: a body left tumbling after a dismount carried the actor around a
circle of about 10 units every physics step.

`RemoteCollision` sets `kNoSim` while the network places the actor and clears
it when handing the actor to Skyrim, including at death. Skyrim's temporary
uses (ID 37314, ID 37652) save and restore it; the AI procedure that clears it
(ID 32797) is followed by the next pass reasserting it.
`Remote actor character body places the actor` and `no longer places the actor`
log each change.

### Pass-through for the local player

Every remote character and skeleton body the local player's proxy phantom
overlaps is ignored for the player's own movement update, which also lets the
player walk through a seated rider's legs
([Native interactions](native-interactions.md#mounts)).

## Engine assumptions

Verified against the 1.7.104 executable:

1. IDs 37534, 37537, and 38561 have the entry bytes above and take `this` in
   `rcx` and the target in `rdx`; 38561 returns in `al` and returns false on
   its own early rejects.
2. ID 38603 takes the attacker in `rcx`, returns nothing, is the only caller of
   38628 in the executable, treats its null result as no target, and calls
   38627 twice. ID 38628 takes the attacker in `rcx` and returns a reference.
   ID 38627 takes aggressor, target, and optional projectile in `rcx`, `rdx`,
   `r8`; its callers are 38603 and 44218, which passes the actor resolved from
   the projectile's shooter. Its first 9 bytes return on a null target, and no
   branch inside it targets bytes +9 to +0x10.
3. ID 39643 has one caller, ID 39642 + 0x32C, which passes the item for the
   speaker in `r8`; 39643 stores it at `middleHigh+0x360` and releases `*rdx`.
4. ID 37577 takes the actor in `rcx` and returns in `al`. ID 42704 has one
   caller, takes the knowledge in `rcx` and a `DetectionState` in `rdx`, and
   matches CommonLib's `ActorKnowledge`.
5. ID 19796 (`TESObjectREFR::ActivateRef`) sends `TESActivateEvent` through
   `ScriptEventSourceHolder` (+0x58) before it tests the reference's blocked
   activation, except for default processing only. For the player it returns
   earlier, without the event, when the base's full name is empty and the
   reference is neither water nor a door; `PlayerLabel::Format` never leaves a
   remote actor's base unnamed, and the vanilla horse is named.

Verified in game:

6. Every hit on a remote actor reaches 39643 and is blocked there (topic
   `00013EBB` and its hit-line infos).
7. With its movement update skipped and `kNoSim` set, a network-placed actor,
   on foot or a mount, stays exactly where the network transform puts it, and
   its 3D follows `SetPosition(..., true)`. A temporary per-pass position check
   found no other writer across replayed mounts and dismounts.

Not yet checked in game:

8. No combat path adds a remote actor to a local combat group's targets without
   passing 37532/37534 or `StartCombat`.
9. Clearing `kPlayerTeammate` does not make the local player's hits start a
   crime or assault reaction. The runtime base has no factions.
10. Bashes and unarmed attacks land through 38603.
11. Every other bark starts through 39642/39643. If one still plays, voice
    playback (ID 37542) is the fallback.
12. Suppressing the actor's detection has no visible side effect, such as
    unreplicated head tracking.
13. A locomotion graph driven by the published Speed plays at the same speed as
    before; `kSupport` set outside `Move` is not overwritten while `Move` is
    skipped; trigger volumes detect actors only through the character body.
14. Death items (ID 37198) were ruled out when remote actors could not die;
    with death synchronized this is unchecked. Damage (ID 37335) is covered by
    the `HandleHealthDamage` guard.
15. The local player's Activate on a remote actor or mount, from the
    crosshair, reaches ID 19796 with the player as the activator, so the
    event above is sent.
16. Precision (read from its source, not run): `RequestPluginAPI` returns the
    same `IVPrecision4` object for V3; its interfaces declare no virtual
    destructor, so `ToggleDisableActor` is slot 23; a disabled actor is
    removed from its active actors and gets no attack collisions; and it
    clears the disabled set only on a player world change.

## Acceptance criteria

1. Bandits, draugr, and predators never detect, draw on, or attack a remote
   actor, whether or not they fight the local player. Guards and townsfolk
   raise no alarm and do not flee because of it. Followers do not attack it.
2. A local actor hit by a remote swing does not enter combat with it. A remote
   melee, bash, unarmed attack, or arrow deals no damage, stagger, hit reaction,
   or impulse to the player, an NPC, or a physics object. With Precision
   installed, the log shows `Precision interface V3 found`, and the same holds:
   a remote swing through the player or an NPC has no hit stop, no hit
   reaction, and no damage.
3. Combat between local actors and the local player is otherwise unchanged.
4. Hitting a remote actor with a weapon, spell, or shout produces no hit line or
   grunt. Near a fight, crime, or sneaking player it speaks no combat, alarm, or
   detection line, and never flees or cowers.
5. Local NPCs still greet, look at, and detect remote actors, and their
   dialogue is unchanged.
6. Hitting a drawn remote actor does not replay its weapon equip animation; the
   sanitization log shows the do-nothing package (27) after the hit, without
   alternating `weaponState` 4 and 2. It still uses furniture after being hit.
7. A remote actor walking through NPCs, a doorway, or clutter, or idling inside
   an object, moves smoothly without shaking, including after a dismount. The
   local player walks through it on foot and riding; local clutter and bodies
   are not pushed.
8. A swimming remote actor logs `supported=false`, swims, and leaves swimming
   when its player wades out (`supported=true`), including up a shallow shore.
9. Placement owners log `owner=furniture`, `owner=mount`, `owner=death`, and
   `owner=network` around each interaction. A dying actor still ragdolls onto
   the ground. A remote actor on a pressure plate does not trigger it.

## Known limitations

- Skeleton (bone) bodies are not isolated: local projectiles, melee, traps that
  test them, and the crosshair still find the remote actor, and it blocks local
  arrows. Isolating them means setting the same bit on every bone body,
  reapplying it after a 3D rebuild, and restoring it on death so the ragdoll
  lands. The player's own movement ignores them, and a seated rider's are
  isolated.
- Local actors' non-combat awareness of the actor (detection, head tracking,
  greetings, pathing) is not suppressed. Explosions and magic from other sources
  than the actor's own draws and casts are not isolated.
- Candidates not adopted, each needing its own in-game check:
  - voice playback (ID 37542, TiltedEvolution's `SpeakSoundFunction`, four
    callers), only if hit lines still play; it mutes every voice line;
  - package selection (IDs 39114 `CheckForNewPackage`, 38959
    `InitFromPackage`), which would remove the frame a reaction package runs;
    TiltedEvolution compiles both out, and `AssertDoNothingPackage` and furniture
    packages must still pass;
  - health regeneration (ID 37448), low value since `SurvivalPolicy` owns
    health;
  - engine position writes (ID 19790), for callers other than the movement
    update.
