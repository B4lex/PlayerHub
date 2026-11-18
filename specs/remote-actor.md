# Remote Actor

## Status

Not run in game: the server's [skeleton pose](#skeleton-pose) limit with more
posed players than `interest.max_posed`; dropping edge probes before a window;
[reaction windows](#source-window) for staggers and kill moves (a paralysis
knockdown has run); [knock isolation](#knockdowns);
[solid remote actors](#solid-remote-actors), the non-default value of
`[Gameplay] DisableCollision`.

## Scope

A remote actor is a local `RE::Actor` that stands in for a player who lives in
another world. This system owns what makes it one: spawning and registration,
readiness, the per-actor engine update, the animation firewall, graph recovery,
survival, isolation from the local world in both directions, the budget of
scene lights its torches and spells take, showing its player's custom
animations through the [skeleton pose](#skeleton-pose), and the engine side of
its [voice](#voice). The local world neither fights, moves, nor is moved by a
remote actor, and the actor's own AI does not react to the local world.

What the actor presents comes from [Locomotion](locomotion.md),
[Combat presentation](combat-presentation.md),
[Appearance and equipment](appearance-and-equipment.md), and
[Native interactions](native-interactions.md). When it is spawned and retired
is decided by the [sync pipeline](sync-pipeline.md#actor-lifecycle).

## Components

| Module | Responsibility |
| --- | --- |
| `actor/ActorSpawner` | Create, remove, and save-exclude a remote actor reference |
| `actor/ActorRuntime` | Readiness and the fixed per-actor engine update, for actors, riders, and mounts |
| `actor/RemoteActor.h` | The replicated instance: handle, readiness, `ActorMovement`, `ActorAnimation`, `ActorFurniture` state |
| `actor/ActorRegistry` | Registered identity and per-actor flags, queried under a lock by hooks on any thread |
| `actor/AnimationFirewall` | Rejects locally injected graph events; network dispatch bypasses it |
| `actor/GraphRecovery` | Detects a local hit reaction and reverts the graph or resets 3D |
| `actor/SurvivalPolicy` | Actor flags, combat sanitization, package takeover, damage, kill, and magic guards |
| `actor/CombatIsolation` | Hostility, combat start, melee and actor hit, knock, and arrow release hooks |
| `actor/SpellIsolation` | Spell fire and cast hooks ([Combat presentation](combat-presentation.md#isolation)) |
| `actor/PrecisionIsolation` | Excludes remote actors from Precision through its interface |
| `actor/RaceMenuOverlays` | Applies and releases a peer's RaceMenu overlays ([Appearance and equipment](appearance-and-equipment.md#overlays)) |
| `actor/ReactionSuppression` | Dialogue, flee, and detection hooks |
| `actor/ScenePackageSuppression` | Scene package action hooks |
| `actor/RemoteCollision` | Placement ownership, the skipped movement update, collision, support, Speed channel |
| `actor/RemoteLights` | Keeps the configured number of remote actors' scene lights lit and culls the rest |
| `actor/ActorLipSync`, `actor/VoiceOcclusion` | Mouth openness and head-to-head occlusion rays ([Voice](#voice)) |
| `runtime/RemoteActorActivation` | The local player's activation of a remote actor: the dead body's prompt and the player menu request |
| `actor/ActorSkeleton` | Reads the local player's third-person bone rotations; blends a remote actor's network pose over its graph's output |
| `actor/ModdedGraph` | Reads the local player's graph layout and its active modded states |
| `actor/GraphVocabulary`, `actor/VanillaGraphNames.h` | The vanilla event prefix and the generated vanilla machine, state, and event name hashes ([Source window](#source-window)) |
| `sync/skeleton/PoseWindow` | Opens, holds, and closes the local player's pose window |
| `sync/skeleton/SkeletonSync` | Samples the local pose inside a window; interpolates and blends a remote actor's pose |
| `runtime/CodeDetour` | Entry and call-site detours ([Detours](#detours)) |

## Spawn

`ActorSpawner::Spawn` creates the reference with `CreateReferenceAtLocation`
at the target position and heading, in the loaded exterior cell containing it
or the local player's interior, as a persistent reference so it survives
exterior cell unloads within one worldspace.

- The base is the runtime `TESNPC` from
  [Appearance and equipment](appearance-and-equipment.md#actor-base).
  `PlayerBase` (`00000007`) is refused: its reference never completes actor and
  3D initialization.
- Activation is blocked, so Skyrim starts no dialogue or interaction and a dead
  body opens no container. Activating a live actor still sends
  `TESActivateEvent`, which opens its player's menu
  ([UI overlay](ui-overlay.md#player-menu-in-the-world)).
- A [dead](native-interactions.md#death) actor shows no activation prompt:
  `RemoteActorActivation` replaces `TESNPC`'s `GetActivateText` (vfunc 0x4C) and
  returns false for a reference `ActorRegistry::IsDead` reports.
- AI stays enabled for the actor's whole life: graph and controller only
  finish initializing and keep updating in Skyrim's normal processing, and
  disabling AI clears `kProcessMe`, zeroes controller velocity, and can stop
  initialization.
- It is registered with `ActorRegistry` right after creation, before any hook
  can see it, and unregistered before the reference is deleted; its
  [Precision exclusion](#precision) follows the same span.
- The reference must not carry `kCollisionsDisabled` (the `TCL` flag).
- The update loop never calls `MoveTo(player)`: an exterior actor may occupy
  another parent cell than the local player.

An actor is ready once its 3D, character controller, and graph manager exist.
`AssertDoNothingPackage` then installs the do-nothing package as its run-once
package ([Package takeover](#package-takeover)).

## Update order

`ActorRuntime::Update` runs once per pass for an on-foot actor:

1. validate the handle (a lost one is reported to the lifecycle);
2. advance a playing network attack (`AttackDispatch::Advance`);
3. `ApplyRemoteReplicaPolicy`, then `SanitizeRemoteCombatState`;
4. recover the graph if a recovery is pending (`GraphRecovery::Recover`);
5. wait for readiness;
6. enable the animation firewall once 3D is loaded;
7. advance an active native furniture interaction, which then owns placement,
   or place the actor by the network ([Placement ownership](#placement-ownership));
8. hold the idle turn state and apply the staged graph state
   (`ActorAnimation::ApplyAnimationUpdate`);
9. attempt a deferred furniture entry, which then owns placement;
10. choose the translation mode ([Locomotion](locomotion.md#translation)), posed
    while a [skeleton pose](#receiver) has weight, and unless it holds, apply
    heading and translation (`ActorMovement::ApplyTranslation`).

A rider and a mount have their own updates (`UpdateRider`, `UpdateMount`,
[Native interactions](native-interactions.md#mounts)).

## Registry

`ActorRegistry` is the only remote-actor state hooks on worker threads read.
Each operation keeps one lock scope; event-name classification in the graph
input hook runs outside it. Hooks on hot engine paths skip the lock when an
atomic count says no actor needs them (gait speed, network-placed actors,
native furniture exits, flying mounts, attack speed scaling).

The lock is a `std::shared_mutex`. Registrations are inserted and erased only
on the main loop, so that thread reads registrations and the fields only it
writes without the lock (`LockUnlessMainThread`, `LockSharedUnlessMainThread`)
and locks exclusively to write a field other threads read (`LockForWrite`).
Other threads lock shared to read main-loop fields or raise an atomic count,
and exclusively to change anything else. Per actor it holds:

- identity and graph holder, indexed by actor, by handle (so AI threads never
  resolve a handle), and by `MagicTarget` and `ActorValueOwner`, so damage and
  magic hooks, which see every actor, find a remote actor without scanning;
- ownership flags that open the firewall: furniture, cast, voice, mounted
  rider, dead;
- `networkPlaced`, the published graph Speed, mount gait speed, flight
  channels, attack speed scale;
- the furniture each actor holds, for the local activation hook
  ([Native interactions](native-interactions.md#shared-furniture)).

## Animation firewall

A hook on `Character`'s `IAnimationGraphManagerHolder::NotifyAnimationGraph`
rejects every graph event local gameplay, AI, or mods send to a registered
actor, logging rejections per event name, throttled. Network presentation calls
the saved graph function directly.

- The firewall is enabled once the graph accepts its initial network state, so
  graph construction is not filtered; it opens while a replacement graph
  initializes and closes once the network state is restored.
- Some native transitions are admitted because Skyrim must run them:

| Admitted | Why | Owner |
| --- | --- | --- |
| `WeapEquip`, `WeapUnequip`, `Equip`, `Unequip` | Native draw and sheathe | [Combat presentation](combat-presentation.md#weapon-stance) |
| `Magic_Equip`, `MagicForceEquip`, `MagicWeap_ForceEquip`, `MagicForceEquipRight`, `MagicForceEquipLeft`, `WeapSoloEquip`, `Magic_Solo_Equip` | Spell hand draw and single-hand swaps | [Combat presentation](combat-presentation.md#spells) |
| `MLh_*`, `MRh_*`, `DualMagic_*`, `RitualSpell*`, names containing `Cast` | While a cast is owned | [Combat presentation](combat-presentation.md#spells) |
| Names containing `shout` or `BreathExhale` | While a shout is owned | [Combat presentation](combat-presentation.md#shouts-and-powers) |
| `SwimStart`, `swimStop` | Skyrim manages swimming locally | [Locomotion](locomotion.md#swimming) |
| `WeapOutRightReplaceForceEquip`, `WeapOutLeftReplaceForceEquip` | Equipping over a drawn weapon | [Combat presentation](combat-presentation.md#weapon-stance) |
| `torchEquip`, `torchUnequip` | A carried torch's arm | [Appearance and equipment](appearance-and-equipment.md#equipment) |
| every event | While furniture, a mount, or death owns the actor | [Native interactions](native-interactions.md) |

- Names compare case-insensitively, as graph events do: the string pool keeps
  the first spelling it interned, and the beast idles spell the sheathe
  differently (`weapUnequip`, `WeapUnEquip`).
- On a mount, Skyrim's gait events are blocked instead, and on a flying mount
  its flight events ([Native interactions](native-interactions.md#mounts)).
- Local attack, hit, cast, combat-target, and AI events, idles the actor's own
  AI or packages request included, never take graph ownership. Network idles go
  through the bypass ([Idle presentation](idle-presentation.md#queue)).
- `ForceDefaultGraphState` sends `IdleForceDefaultState` past the filter, then
  raises a carried torch again, since the reset drops the torch arm layer.

### Graph channels

Skyrim copies each actor's own movement into graph variables during every
animation update, after main-loop writes. For a registered actor the hooks
report network values instead: `ActorDirectionChannel` the network direction,
`ActorTurnDeltaChannel` 0, and `ActorSpeedChannel` the graph Speed
`ActorAnimation` published, or a mount's or rider's gait speed
([Graph speed](#graph-speed)).

A flying mount's `ActorTurnDeltaChannel` reports its turn rate, and
`ActorTargetSpeedChannel` and `ActorPitchDeltaChannel` its target speed and
pitch rate (`ActorRegistry::GetFlightChannels`,
[Native interactions](native-interactions.md#dragon-flight)); for other
actors these two keep Skyrim's value. They share the weapon speed channels'
layout (vfunc 1, IDs 38331 and 38328, SE vtables 260607 and 260612).

`ActorWeaponSpeedChannel` and `ActorLeftWeaponSpeedChannel` keep the actor's
own value multiplied by its attack speed scale while attacks catch up
([Combat presentation](combat-presentation.md#catching-up)). Their Actor is at
0x20 with no cached value, and vfunc 1 rewrites the value at 0x18 on every poll,
so the hook scales it in place. On SE the layout is not checked; a different
offset would only make the pointer-comparing registry lookup find nothing.

### Graph recovery

Some hit reactions enter the graph below `NotifyAnimationGraph`. A
`TESHitEvent` for a registered actor, or stagger or recoil state seen by
polling, queues a recovery; the hit callback never touches the actor. On the
next update the firewall opens and `RevertAnimationGraphManager` (ID 32883,
SE 32139) resets the whole graph; `DoReset3D(false)` is the fallback when the
revert fails or the graph is not ready within 2 s. A recovery, or a detected
graph manager replacement, invalidates all dispatch bookkeeping. Translation
pauses until graph and controller have been ready for two consecutive passes;
then the newest checkpoint is replayed and a held idle re-entered
([Idle presentation](idle-presentation.md#re-entry)).

## Survival and AI policy

- `ApplyRemoteReplicaPolicy`, every update, clears `kPlayerTeammate`,
  `kCanDoFavor`, `kCanSpeak`, `kAttackOnSight`, and `kAngryWithPlayer`, and
  disables attacking and casting. Presentation clears `kAttackingDisabled` or
  `kCastingDisabled` only for one synchronous dispatch; a held cast or shout
  keeps `kCastingDisabled` clear.
- Immediately before Skyrim's actor update, the AI process is marked as
  ignoring combat and any combat controller inactive; the update still runs.
- `SanitizeRemoteCombatState`, once per pass, stops combat, clears flee flags,
  stops alarms, resets a fear emotion, takes back any engine-selected package
  ([Package takeover](#package-takeover)), and clears the do-nothing
  procedure's target ([Do-nothing procedure target](#do-nothing-procedure-target)).
- Guards on `KillImpl`, `KillDying`, `HandleHealthDamage`, and health
  `ModActorValue` keep the actor alive. A magic firewall,
  `MagicTarget::AddTarget` on `Character`, refuses a hostile or detrimental
  effect, and any shout's (`kVoicePower`) effect unless the actor cast it on
  itself, since effects through projectiles or explosions do not always keep
  their caster. Each update dispels an effect the hook missed by the same rule
  (`Dispelling magic effect that bypassed the remote actor firewall`). A dead
  registration lets these through and stops reasserting the policy
  ([Native interactions](native-interactions.md#death)).

## Combat isolation

Local actors never become hostile to a remote actor, target it, or start combat
with it. Its replicated attacks hit nothing and the local player's attacks on
it do not land, whatever the `[Gameplay]` settings. Nothing local staggers or
knocks it down ([Knockdowns](#knockdowns)). Its native spells show their
projectiles but act on nothing
([Combat presentation](combat-presentation.md#spell-world-isolation)).

In 1.7.104 the hostility decision (ID 37534) replaces a `kPlayerTeammate`
target with the player, so with the flag every actor hostile to the player
would be hostile to every remote actor, and its wrapper ID 37532 returns true
for a teammate outright. The policy clears the flag, but very aggressive
actors attack neutral targets too, so hooks decide.

### Hooks

Hooked at their entry, so every caller, AI threads included, sees the same
answer:

| ID | Function | Moved entry bytes | A remote actor on either side |
| --- | --- | --- | --- |
| 37534 | Hostility decision `(actor, target, aggression, mode)` | `mov [rsp+20h], r9b` | returns false |
| 37537 | `Actor::IsHostileToActor` (faction-reaction path) | `mov [rsp+10h], rbx` | returns false |
| 38561 | `Actor::StartCombat` (also reached from hits) | `mov r11, rsp; push rbp; push rsi` | returns false |

The wrappers 37532 and 37533 tail-call 37534, which also holds the frenzy
check, so frenzied actors ignore remote actors.

Remote attack animations play natively, so their `HitFrame` annotations run the
local hit pipeline. Three more hooks drop hits:

| ID | Function | Hook | Drops when |
| --- | --- | --- | --- |
| 38603 | Melee hit frame `(attacker, leftHand, flag)`: reach sweep, the swing's target (38628), the actor hit (38627) for each swept actor and the target, or the object hit (38620) | entry, `mov rax, rsp; mov [rax+10h], dl` | the attacker is remote; returns without sweeping |
| 38628 | Swing hit target `(attacker)` (Precision's `CalculateCurrentHitTargetForWeaponSwing`) | entry, `mov rax, rsp; mov [rax+8], rcx` | the same; returns null |
| 38627 | Actor hit `(aggressor, target, projectile, leftHand)`: damage, stagger, hit effects, `TESHitEvent` | at +9, `mov rax, rsp; mov [rax+8], rcx` | the target is remote, or the aggressor is and the hit has no projectile or `GameplayPolicy::MayAffectWorld` refuses its source ([Isolation settings](combat-presentation.md#isolation-settings)) |

The melee hook also covers the queued path (ID 36915 defers to task ID 36991,
which calls 38603 again); bashes and power attacks reach the same hit frame.
The actor hit is filtered at its entry because combat mods call it directly
(Precision calls 38628 and 38627 without 38603); its callers in the executable
are 38603 and the projectile impact (ID 44218 + 0x90), so a local arrow
striking a remote actor is dropped too. 38627 opens with a `test rdx, rdx; je`
guard whose relative jump cannot move into a gateway, so the detour sits after
it.

### Knockdowns

`AIProcess::KnockExplosion` (ID 39895, SE 38858), which staggers or ragdolls an
actor away from a point, is hooked at its entry (`mov [rsp+18h], rbx`) and
returns without acting for any registered remote actor, alive or dead, rider or
mount (`Suppressed knock of remote actor`). Its callers include Papyrus
`PushActorAway` (ID 56213), explosions (IDs 43849, 43853), hit processing
(ID 38586), and the deferred task processor (ID 36991). Push mods such as Get
Out of My Way pass the [magic firewall](#survival-and-ai-policy), and a local
ragdoll fights the network placement every pass. The source's own knockdowns
reach peers through [pose windows](#source-window).

A call from a thread that may not knock (a Papyrus VM thread) is queued as a
task (ID 36917) that comes back through `KnockExplosion`, so the thunk may run
off the main thread; it only reads the registry, shared.

### Precision

Precision replaces melee hit detection with weapon collision bodies and applies
its own hit stop, impulses, and reactions before calling the actor hit.
`PrecisionIsolation` takes remote actors out of it, so they fall back to the
vanilla hit frame, which the hooks drop:

- `Initialize` at `kDataLoaded` calls `Precision.dll`'s `RequestPluginAPI` with
  `InterfaceVersion::V3` and keeps `ToggleDisableActor` (virtual slot 23).
  Without the module nothing else runs; without V3 a warning is logged and the
  actor hit hook is the only guard.
- `ActorSpawner::Spawn` excludes each remote actor and mount, and `Remove`
  releases it; a disabled actor is never added to Precision's world.
- Precision clears its disabled actors when the player's `bhkWorld` changes and
  re-adds actors after `fWorldChangedWaitTime` (0.4 s), so
  `PrecisionIsolation::Update` reasserts every exclusion each pass for 1 s
  after the player cell's world changes.

### Detours

`CodeDetour` copies the first entry bytes, ending at an instruction boundary
and position independent, into an SKSE trampoline gateway that jumps back, then
writes a 5-byte branch to the thunk; an offset moves the detour past an entry
guard that only returns. The bytes are compared first: if another plugin
patched them, the hook is skipped with an error. A plugin that later hooks the
same entry by copying its first bytes breaks, so a function with few callers is
detoured at its call sites instead
([Spell world isolation](combat-presentation.md#spell-world-isolation)); a
call-site detour chains to whatever the call pointed to. `WriteCalleeDetour`
finds the site as the caller's first call to the callee, scanning to the end of
the caller's code as `RtlLookupFunctionEntry` reports it, including following
`.pdata` chunks chained to it (`UNW_FLAG_CHAININFO`; ID 34451's launch call is
in one). A site another plugin detoured first is not found and the hook is
skipped with an error. Startup logs which hooks were installed; suppressions
are logged with path, form IDs, and a count, throttled.

## Reaction suppression

A remote actor keeps full actor processing, so its AI still evaluates the local
world. Flags reasserted from the main loop remove a reaction only after the
engine chose it, and some paths ignore them (a hit remote actor spoke hit lines
with `kCanSpeak` cleared), so three functions are hooked where the reaction is
decided:

| ID | Function | Moved entry bytes | Registered remote actor |
| --- | --- | --- | --- |
| 39643 | AI process dialogue response `(process, DialogueItem by value, speaker, listener, flag)` | `mov rax, rsp; mov [rax+10h], rdx` | as speaker: returns without starting it and releases the item, except a shouted word ([Combat presentation](combat-presentation.md#voice-state)): subtypes `kVoicePowerStartShort`, `kVoicePowerStartLong`, `kVoicePowerEndShort`, `kVoicePowerEndLong` |
| 37577 | `Actor::IsFleeing` | `push rbx; sub rsp, 20h` | returns false |
| 42704 | `ActorKnowledge::SetDetectionState(state)` | `push rbx; sub rsp, 20h` | as knowledge owner: returns without storing or notifying |

- The dialogue thunk forwards all five arguments. The item is a by-value copy
  the caller (ID 39642) took a reference for, so a blocked call releases it
  through the engine destructor (ID 35221) and sized delete (ID 109588);
  CommonLib's defaulted destructor would not free the responses. The log names
  speaker, listener, topic, and topic info.
- `IsFleeing` calls the original and logs only calls that would return true.
- 42704 takes its own reference to the state, so skipping releases nothing.
  The owner is compared by handle; the remote actor detects no one.

`ApplyRemoteReplicaPolicy` and the combat sanitization stay beside these hooks:
each flag they clear also guards paths the hooks do not touch.

### Package takeover

A hit makes the engine select `DefaultMasterPackage` (`0003EAB9`, type 18),
which wants a sheathed weapon while the network holds it drawn, so the two
would fight every update. `SanitizeRemoteCombatState` therefore restores the
do-nothing package whenever the actor runs any other, once ready
(`a_mayResetPackage`), furniture included: furniture runs on the do-nothing
package, and installing it neither reaches `ClearFurniture` (ID 39798) through
`PutCreatedPackage` or `SetRunOncePackage` nor runs `PutCreatedPackage`'s
sit-state handling for that type. Each takeover logs `Took back local actor
package` with type and form.

A package the engine reselects every pass would never converge, as with the
bard audience quest's alias packages, which also stole furniture occupancy:

- `Actor::CheckForCurrentAliasPackage` (vfunc `0x49`) returns null for a
  registered actor: a remote actor is never a participant in a local quest.
- `AssertDoNothingPackage` installs the do-nothing package as the run-once
  package, which outranks quest and scene packages and survives reselection;
  every path handing the actor back to synchronization uses it instead of
  `InitiateDoNothingPackage`.

### Do-nothing procedure target

The do-nothing package's procedure (ID 39268) requests a sheathe whenever the
weapon is out, and once sheathed, with an `AIProcess::target`, faces it and
requests a `kActionIdle` whose event the firewall blocks
(`MotionDrivenIdle`), so the procedure never completes. Its sheathe request
then fails mid-equip, leaving `kWantToSheathe`, and the weapon transition draws
again, looping the equip clip. `SanitizeRemoteCombatState` clears
`AIProcess::target` whenever the current package is the do-nothing type;
nothing in the plugin relies on it. Debug logs, throttled:

- `Cleared local AI target from remote actor` names the target, weapon state,
  and current idle.
- `Remote actor weapon state opposes the network` fires when `ActorState` heads
  away from the authoritative state (`kWantToSheathe` or `kSheathing` while
  drawn is wanted, `kWantToDraw` or `kDrawing` while sheathed is), naming the
  package type, process target, and idle. `WeaponTransition` only requests the
  network's state, so these come from local AI or a sheathe still playing.

### Scene packages

A scene's package action also installs packages on the alias's actor, outside
package evaluation. On 1.7.104:

- `BGSSceneActionPackage` vfunc `0x10` (ID 24244) starts the action, resolving
  the alias's actor (ID 25066); with no actor it marks the action started and
  ends it through vfunc `0x11` (ID 24245).
- ID 24252 reinstalls the package whenever the actor runs another, called from
  ID 24130 + `0x9A` and ID 24173 + `0x61`. ID 24253 installs the same way but
  has no reference and is not hooked.

On SE the start is ID 23792, the end 23793, and the install ID 23800, called at
ID 23709 + `0xB0`, ID 23704 + `0x61`, and by the start action itself.
`ScenePackageSuppression` refuses both for a registered actor (`Refused local
scene package`, with scene, quest, alias, and package); the start hook resolves
the alias through `TESQuest::GetAliasedRef` and treats a remote actor like an
alias without an actor.

## World isolation

The network transform is the only thing that places a remote actor. If each
update integrated its character controller from where the network put it, root
motion would add, the controller would push out of overlaps and push local
bodies, and the next pass would move it back, so it would shake.

### Placement ownership

`ActorRuntime` decides every update who places the actor, and `RemoteCollision`
publishes it (`networkPlaced`):

- an on-foot actor is network-placed once ready, except while native furniture
  owns it;
- a mount is network-placed once ready;
- a rider is left to Skyrim, which carries it on its mount;
- a dead actor is left to Skyrim's death and ragdoll; `ActorDeath::Kill`
  releases it (`owner=death`), and the registry's `dead` overrides
  `networkPlaced`.

`Remote actor placement owner changed` logs each change.

### Skipped movement update

The hook on the movement update call (ID 37350 + 0xFB, the only call of
ID 37356; SE 36359 + 0xF0 calling 36365) skips it for a network-placed actor
and passes through for any other remote actor. ID 37356:

1. reads root motion and the movement controller's output through
   `ModifyMovementData` (vfunc 0x11A) and stores the process movement speed
   (`high + 0x2A8`, ID 39873);
2. applies the heading change (`SetHeading`, ID 37230);
3. calls `Actor::Move(delta, translation)` (vfunc 0xC8, ID 37507), which
   integrates the controller, or with TCL adds the translation through
   `SetPosition`;
4. unless the actor flies, feeds requested against actual motion into the
   graph (vfuncs 0x11D/0x11E).

Skipping only `Move` would leave step 4 measuring zero motion; skipping the
whole update removes the second writer of position and heading.

### Frame order

The main-loop hook wraps the call at ID 36564 + 0x6E (ID 40438), which reaches
the per-frame actor movement job list (ID 41306, job callback ID 41444, calling
ID 37350), so Skyrim's movement update runs before the pass and the network
transform is the last write before render. The movement update runs on job
threads, so `IsNetworkPlaced` locks shared, skipped by an atomic count when no
actor is network-placed.

### Graph speed

The Speed channel (ID 38330 → 42767) reads `Actor::GetMovementSpeed`
(ID 37957 → 39872), which step 1 stores and a network-placed actor no longer
updates, so `ActorAnimation` publishes the Speed it writes and the channel hook
reports it for a living network-placed actor. The hooks replace only the polled
value at `channel + 0x18`. The Speed channel divides every polled speed by the
actor's scale kept at `+0x24` (set by its reset, vfunc 2, ID 42768, falling
back to 1.0); the hooks leave it alone, since a network speed written there
would divide later engine-driven speeds by it (0 for an idle actor).

### Swimming

`Character::Update` (vfunc 0xAD, ID 37348) decides swimming from the submerged
fraction (ID 37448), which follows the network position: at 0.7 or deeper it
starts swimming (ID 37946) and requests controller state 5; below 0.7 it stops
only when the controller's `kSupport` (`controller + 0x218`, bit 8) is set or
its state (`context + 0x20`) is in-air. These requests (`wantState`,
`controller + 0x21C`) apply only inside `Move`, so with `Move` skipped an actor
would keep swimming on land. `RemoteCollision::PlaceByNetwork` sets `kSupport`
whenever the checkpoint's `swimming` is false and clears it while true, so the
engine's own exit ends swimming once the actor is shallower than 0.7 and its
source stopped. An actor deeper than 0.7 in local water keeps swimming.
`Remote actor controller support changed` logs each change.

### Character body

Skyrim gives every non-player actor a rigid-body controller (ID 37171 builds
`bhkCharProxyController` only for the player or on request), whose character
body (`GetRigidBody`, ID 79189, `+0x350`) is simulated whether or not the
movement update runs, so it collides with and pushes local bodies and triggers
local volumes.

`RemoteCollision` sets Havok's no-collision filter bit (14) on that body while
the network or a mount places the actor, and clears it whenever Skyrim moves the
actor, so a seated or dying actor still stands on the world. Each pass
reasserts it, since a rebuilt controller collides again. The filter core
(ID 78553) rejects any pair where either side has the bit, unless the first
side's layer is 0x2C. The bit is set without `updateCollisionFilterOnEntity`,
so it applies to new contacts and queries. `Remote actor character body
collision disabled` and `restored` log each change.

### Body write-back

Every physics step, the controller's post-simulation callback (ID 79185, second
vtable) sends the controller position (ID 79194) through the move-finish event
source (`controller + 0x10`, ID 38322) to the actor's sink (`actor + 0xD8`),
which places the actor there, unless `kNoSim` (bit 17 of `controller + 0x218`)
is set; a body left tumbling after a dismount would carry the actor in circles.
`RemoteCollision` sets `kNoSim` while the network places the actor and clears it
when handing the actor to Skyrim, death included. Skyrim's temporary uses
(IDs 37314, 37652) save and restore it, and the next pass reasserts it after the
AI procedure that clears it (ID 32797). `Remote actor character body places the
actor` and `no longer places the actor` log each change.

### Solid remote actors

Goal: while `[Gameplay] DisableCollision` is off, a network-placed remote actor
or mount collides as a local NPC does: it blocks the local player and NPCs,
stops arrows and spells, pushes clutter, and sets off local traps, pressure
plates, and trigger volumes, an exception to world isolation the player opts
into. Non-goal: colliding with only the player and arrows, which would need a
detour at each of the collision comparison's seven callers and a per-group
registry.

- `RemoteCollision::PlaceByNetwork` then clears the no-collision bit on a
  network-placed actor's body. A rider stays isolated; furniture and dead
  actors are Skyrim's.
- The body still does not place the actor (`kNoSim`), and `SetPosition` warps
  it to the network position every pass, so a push moves only the body until
  the next pass.
- The player's arrows stop on the actor through that body (`L_PROJECTILE`
  collides with `L_CHARCONTROLLER`, not a live actor's `L_BIPED` bones), and
  the actor hit hook drops their hit ([Hooks](#hooks)).

### Pass-through for the local player

Every remote character and skeleton body the local player's proxy phantom
overlaps is ignored for the player's own movement update, which also lets the
player walk through a seated rider's legs
([Native interactions](native-interactions.md#collision)), and the character
body any moving controller bumped is ignored for that update when either side
is remote. While remote actors are solid, character bodies are left out of
both; skeleton bodies are still passed through for the player.

## Light budget

Skyrim lights each geometry with a few dynamic lights only, so many remote
players carrying torches or spells make the world's lights flicker or drop out.
`[Advanced] LightsLimit` ([Configuration](configuration.md#advanced), default
0, `0..8`) caps how many of their lights light the scene. A culled light loses
only its light; flames, hand art, and sounds stay. Shadow-casting torches and
hand lights are already [fake lights](appearance-and-equipment.md#fake-lights).

`RemoteLights::Update` runs at the end of `SyncCoordinator::OnFrame` with the
world camera's position, returning at once while there are no peers and nothing
is culled. Under the scene's `lightQueueLock` it:

1. walks `ShadowSceneNode` `activeLights` and `activeShadowLights`, counting a
   light when its `NiLight`'s `GetUserData` is a registered remote actor (so
   torches, hand lights, and lights in modded meshes or art all count). A light
   the engine already keeps dark (app-culled, or fade below 0.05) takes no slot
   unless this module culled it;
2. ranks the counted lights by squared camera distance, a lit light's scaled by
   `kLitRankScale` (0.64), so it keeps its slot until an unlit one is a fifth
   closer;
3. keeps the nearest `LightsLimit` lit (`std::nth_element`) and app-culls the
   rest's `NiLight`, re-setting the flag every pass; a culled light now kept
   gets the flag cleared;
4. clears and forgets every culled light not counted this pass, so no light
   stays dark untracked.

A culled light is held as `NiPointer<BSLight>`, never its `NiLight`: the engine
drops a light once its `BSLight` holds the last reference to the `NiLight`, so
an extra one would leave it behind after its torch or spell is gone. `Remote
light budget` logs the limit and counts whenever they change.

## Voice

[Voice chat](voice-chat.md) calls these on the game thread
(`GameThread::Require`) from `VoiceChatSync::UpdatePeers`:

- `ActorLipSync::Apply` takes an openness from 0 to 1 and writes `Aah` (0.75 of
  it) and `BigAah` (0.45 of its square) in the actor's
  `BSFaceGenAnimationData::phenomeKeyFrame` under its `lock`, as the console's
  `mfg phoneme` does; 0 closes the mouth. An actor without face data, or a
  keyframe with fewer phonemes, is left alone.
- `VoiceOcclusion::IsBlocked` casts one `bhkWorld::PickObject` ray between two
  points in the player's cell's Havok world on the character controller layer,
  keeping the closest static, animated static, terrain, ground, or prop hit and
  passing characters, water, and clutter.

## Skeleton pose

A remote actor shows animations its player's game plays that the vanilla
reproduction does not cover by taking the bone pose its player's game animated:
new graph states from mods such as Paraglider or SkyParkour, modded idles, and
the reactions the reproduction leaves out (staggers, the player's own kill
moves, knockdowns with ragdoll and get-up). Poses travel only during such an
animation (a pose window), so everything the reproduction covers keeps syncing
through the graph and a viewer's replacers apply. Viewers with and without the
mod see the same pose. The graph keeps running the vanilla reproduction
underneath, so weapon draws, sounds, footsteps, and spell art still come from
it, and bones the pose leaves out keep the graph's output. Positions stay on
checkpoints.

Non-goals:

- Replacers of vanilla clips (OAR and DAR variants) never open a window; they
  sync natively. A replacer of a reaction's clip shows through that reaction's
  window as the source played it.
- Modded behavior entered by vanilla events under modded conditions (MCO and
  SCAR attack chains) and modded idle forms played through vanilla events.
- Other vanilla states the reproduction leaves out: bleedout, attack recoils,
  ritual spells, non-lethal paired animations; flinches and block hit reactions
  layer over locomotion and fire too often for a whole-body pose.
- A clip's side effects: AnimObjects (Paraglider's glider), `SoundPlay.*`
  annotations, contrails.
- Beast forms, furniture, mounts, and death keep the graph. A player killed by
  a kill move ragdolls on peers like any death, since its paired death belongs
  to an attacker only in its own world.
- Replaying modded behavior natively on viewers whose graph has it, which would
  show the viewer's variant and side effects at the cost of per-mod state.

### Source window

`sync/skeleton/PoseWindow` decides when the local player's checkpoints carry a
pose; `SkeletonSync::SampleLocalSkeleton` returns an empty pose outside a
window. Every client shares its windows; the server bounds what each recipient
is sent ([Interest and delivery](interest-and-delivery.md#skeleton-poses)).

- **Modded event.** An accepted event no vanilla behavior lists is modded
  (`PoseWindow::IsModdedEvent` looks up its `GraphVocabulary::HashName` in the
  generated vanilla event names, on the graph input hook's thread). Windows
  open only in a third-person graph (`BSAnimationGraphManager::graphs[0]`) with
  the vanilla prefix (`GraphVocabulary::HasVanillaPrefix`: the last vanilla
  event and variable are `SoundPlay.NPCKillDLC2HeartStone` at 1216 and
  `iWantMountedWeaponAnims` at 229), read whenever that graph changes (`Pose
  window observed local graph`).
- **Open.** The graph input hook passes each accepted event to
  `PoseWindow::ObserveLocalGraphEvent`, which keeps the first modded name and a
  count until the next pass. `PoseWindow::Update`, before idles and the
  checkpoint, opens a window on a pass that saw one (`Pose window opened`),
  unless the client is disconnected or the player is in a beast form,
  furniture, mounted, or dead; any of these closes an open window. Repeated
  modded events extend nothing on their own (Paraglider sends `StartPara`
  every frame).
- **Reactions.** On a pass without a window, a reaction also opens one
  (`reaction=` and the life state): `kill-move` while `Actor::IsInKillMove`
  (the dead block limits it to the attacker), `knockdown` while the knock state
  is not `kNormal` or the actor is ragdolled (`IsInRagdollState`, also set by
  paralysis), `stagger` while `ActorState::IsStaggered` or the graph's
  `IsStaggering` holds (a script's `staggerStart` sets the latter). A reaction
  holds the window like a modded state, without the 1 s minimum, and its end
  closes it after the tail (`reaction-ended`); a modded window entering a
  reaction logs `Pose window entered reaction`.
- **Hold.** Every 0.1 s of clamped frame time,
  `ModdedGraph::ReadModdedState` reads the active state machines under the
  manager's `updateLock`: a machine whose name is not vanilla, or a vanilla
  machine in a non-vanilla state, is modded (`Pose window entered modded
  state`). The window holds while one is active or a reaction is, and one opened
  by a modded event holds at least 1 s, since a machine switches on the graph
  update after the event and a modded event can play through vanilla states.
- **Close.** After 0.25 s past that without a modded state or reaction, the
  window closes (`Pose window closed` with `graph-vanilla`, `reaction-ended`, or
  `graph-unobserved`), so the last poses show the vanilla state the receiver
  reproduces. It also closes on an epoch or graph change, a block above, a
  disconnect, or leaving the world, never on a modded exit event (Paraglider's
  landing leaves on the vanilla `JumpLandEnd`).
- **Vanilla names.** `actor/VanillaGraphNames.h` holds the sorted FNV-1a 64
  hashes of the ASCII-lowercased machine, state, and event names of the vanilla
  character behaviors (1056 machine, 3246 state, 1984 event names; `0_master`
  lists 1217 events), looked up by binary search.
  `scripts/generate_vanilla_graph_names.py` generates it from every `.hkx` under
  `actors\character\behaviors` and `actors\character\_1stperson\behaviors` of
  the vanilla `Skyrim - Animations.bsa` (v105, LZ4) or an extracted directory,
  parsing each hk_2010.2.0-r1 packfile: virtual fixups give each object's
  class, and the local fixups of `hkbStateMachine::name` (0x38),
  `StateInfo::name` (0x60), and `hkbBehaviorGraphStringData::eventNames` (0x10)
  the names. It checks each `0_master`'s 1217 events end in
  `SoundPlay.NPCKillDLC2HeartStone` and fails on a hash collision. Run
  `python3 scripts/generate_vanilla_graph_names.py "<Data>/Skyrim -
  Animations.bsa"` (needs `lz4`).
- **Always-active machines.** Outside windows,
  `ModdedGraph::CollectModdedMachines` samples every 0.1 s, and a modded machine
  active in every sample for 2 s counts as vanilla for that graph from then on
  (`Pose window treats an always active modded machine as ordinary`), so a
  patch's always-on machine does not hold every window.
- **Idles.** A modded `Idle*` event opens a window instead of reaching
  [idle sync](idle-presentation.md#capture), and an opening window ends the
  held vanilla idle.

### Receiver

- **Posed peers.** A peer is posed while its played checkpoint carries a pose in
  its human form. The client poses every such peer; the server limits them to
  the nearest `interest.max_posed` within its near distance
  ([Interest and delivery](interest-and-delivery.md#skeleton-poses)). A
  hand-over at the limit can show one more for the interpolation delay.
- **Blend.** A posed peer blends from the graph's output to its pose over
  0.2 s (`Remote skeleton pose blending in`) and back over 0.2 s, holding the
  last pose, once poses stop, then releases the actor (`Remote skeleton pose
  released to the graph`). The weight follows clamped frame time; turning it on
  mid-way continues from the current weight.
- **Ownership.** After `ActorRuntime::Update`, `SyncSkeleton` poses a ready
  actor on foot in its human form outside furniture. At death and on a mount,
  `ReleaseSkeleton` clears the pose at once, as `ActorRegistry::Unregister`
  does; a respawned actor starts from weight 0.
- **Interpolation.** The pose moves from the played checkpoint's toward
  `RemoteFrame.next`'s at `RemotePose.fraction`
  ([Sync pipeline](sync-pipeline.md#checkpoint-playout)), normalized-linear per
  bone present in both (`Network::NlerpRotation`). `SkeletonSync::RemoteState`
  keeps both decoded poses by sequence, so a checkpoint decodes once.
- **Placement.** While the pose has weight, the actor stands at the
  interpolated network position and, at full weight, heading
  ([Locomotion](locomotion.md#translation)), without the holds and catch-up
  built for the vanilla reproduction.
- **Ground.** A posed checkpoint holds the
  [ground offset](locomotion.md#ground-offset), since a climb keeps its source's
  controller grounded while rising a ledge.
- The [animation firewall](#animation-firewall) keeps rejecting modded events
  local mods send to the remote actor.

### Pose

- **Bones.** `Network::kSkeletonBoneNames`: 63 bones of the vanilla human
  skeleton (COM, pelvis, legs and toes, spine, neck, head, arms with twist
  bones, all 30 finger bones, `Weapon`, `Shield`). `NPC Root`, skirt, camera,
  look, magic, `AnimObject`, sheath, physics, and modded bones are left out.
  Only COM's translation travels, so the receiver's bone lengths stay.
- **Matching.** Bones match by name in the graph's Havok skeleton
  (`characterInstance.setup->animationSkeleton`), whose indices are
  `boneNodes` indices; a bone either side lacks is skipped. The mapping is
  rebuilt when the graph, skeleton, or bone count changes (`Remote skeleton
  pose mapped a graph`, `Local skeleton pose mapped the third-person graph`).
- **Source.** `ActorSkeleton::ReadThirdPersonPose` takes the graph whose
  `rootNode` is `Get3D(false)`, under `updateLock`, and reads each mapped
  bone's local rotation in the pass that publishes the checkpoint. In first
  person the third-person graph keeps copying its pose and is driven by the
  same events.
- **Ragdoll.** While the player is ragdolled (`IsInRagdollState`), Skyrim
  moves the reference with the body while COM's local stays nearly fixed, so
  bone locals alone carry neither tilt nor fall. `SampleLocalSkeleton` then
  reads COM in `ActorSkeleton::RootSpace::kReference`: its world transform
  relative to an upright frame at the actor's position and heading, translation
  divided by its parent's world scale. The receiver applies it as COM's local
  under its upright root at the same position and heading. Outside a ragdoll COM
  keeps its local (`kParent`); upright, both read the same. A change logs `Local
  skeleton pose root space`. The source also clears `grounded` while ragdolled
  ([Locomotion](locomotion.md#source-capture)).
- **Hook.** The pose copy of `BShkbAnimationGraph` (ID 63589, SE 62644) writes
  the generated pose into `boneNodes`' local transforms through ID 63856
  (SE 62933). `ActorSkeleton` wraps its calls at ID 63358 + 0x319 (SE 62416 +
  0x267), the manager update, which calls it for every graph under
  `updateLock`, and at ID 63359 + 0x77 (SE 62417 + 0x66). After the copy, a
  posed holder's present bones get their rotations, and COM its translation,
  written over (below weight 1, blended from the copied locals), before Skyrim
  derives world transforms. A bone entry's local transform is found as ID 63856
  finds it: a nonnegative index at +0x08 selects a 0x80-byte slot in the array
  at the node's +0x130, whose object at +0x70, or the slot itself, holds it. The
  slot is a `BSFlattenedBoneTree::BoneEntry`, whose world transform at +0x34
  the reference root space reads.
- **Ragdoll copy.** While a graph's ragdoll is in the physics world (ID 63564),
  ID 63589 skips the copy; ID 63587 queues the graph, and after the physics step
  ID 63563 runs the ragdoll driver, copies into the locals through ID 63856, and
  applies its world-from-model (ID 63569).
- **Threads.** The hook runs on the graph's update thread. Poses and weights
  live in a map keyed like `ActorRegistry` under a shared mutex the hook takes
  shared, skipped by an atomic count while no actor has a pose; the main loop
  replaces an actor's pose each pass exclusively.

### Budget

A whole pose is 211 B and a [change](network.md#pose-changes) averaged 87 B on
a recorded window, so a posed pair costs about 1.8 KB/s at 20 Hz, only during
windows. At the default `max_posed` of 32 a client receives about 56 KB/s of
poses at most (135 KB/s were every pose whole), however many players pose
around it.

## Engine assumptions

1. IDs 37534, 37537, and 38561 have the entry bytes above and take `this` in
   `rcx` and the target in `rdx`; 38561 returns in `al` and false on its own
   early rejects.
2. ID 38603 takes the attacker in `rcx`, is the only caller of 38628, treats a
   null result as no target, and calls 38627 twice. ID 38627 takes aggressor,
   target, and optional projectile in `rcx`, `rdx`, `r8`; its callers are 38603
   and 44218 (the actor resolved from the shooter). Its first 9 bytes return on
   a null target, and no branch targets bytes +9 to +0x10.
3. ID 39643 has one caller, ID 39642 + 0x32C, which passes the item for the
   speaker in `r8`; it stores it at `middleHigh+0x360` and releases `*rdx`.
   Every hit on a remote actor reaches it.
4. ID 37577 takes the actor in `rcx` and returns in `al`. ID 42704 has one
   caller, takes the knowledge in `rcx` and a `DetectionState` in `rdx`.
5. `TESObjectREFR::ActivateRef` (ID 19796) sends `TESActivateEvent` through
   `ScriptEventSourceHolder` (+0x58) before testing blocked activation, except
   for default processing only; for the player it returns earlier without the
   event when the base's full name is empty and the reference is neither water
   nor a door (`PlayerLabel::Format` never leaves a base unnamed; the vanilla
   horse is named). The blocked test (`ExtraFlags` bit 0) skips the base's
   `Activate` (vfunc 0x37) unless it is the player's own occupied furniture.
   The crosshair update (ID 40621, SE 39535) calls the base's
   `GetActivateText` (vfunc 0x4C; `TESNPC`'s is ID 24716, SE 24212) and shows
   no prompt when it returns false. The local player's crosshair Activate on a
   remote actor or mount reaches ID 19796 with the player as activator.
6. With its movement update skipped and `kNoSim` set, a network-placed actor
   stays exactly where `SetPosition(..., true)` puts it, with no other writer.
7. No combat path adds a remote actor to a local combat group's targets without
   passing 37532/37534 or `StartCombat`, and clearing `kPlayerTeammate` starts
   no crime or assault reaction (the runtime base has no factions).
8. Bashes and unarmed attacks land through 38603.
9. Every other bark starts through 39642/39643; otherwise voice playback
   (ID 37542) is the fallback.
10. Suppressing detection has no visible side effect, such as missing head
    tracking.
11. A graph driven by the published Speed plays at the speed the actor's own
    movement would drive; `kSupport` set outside `Move` is not overwritten while
    `Move` is skipped; trigger volumes detect actors only through the character
    body.
12. The death items path (ID 37198) needs no guard; damage (ID 37335) is
    covered by the `HandleHealthDamage` guard.
13. Precision (read from its source): `RequestPluginAPI` returns the same
    `IVPrecision4` object for V3; its interfaces declare no virtual destructor,
    so `ToggleDisableActor` is slot 23; a disabled actor gets no attack
    collisions; the disabled set clears only on a player world change.
14. The SE 1.5.97 counterparts, matched by instruction sequence and callees:
    combat 36534 (37534), 36537 (37537), 37608 (38561), 37650 (38603), 37674
    (38628), 37673 (38627), 38858 (39895); spell fire 33629 (34407); reactions
    38612 (39643), 34414 (35221), 102158 (109588), 36575 (37577), 41626 (42704).
    Hostility, IsHostileToActor, spell fire, the swing target, and the reaction
    detours have the same entry bytes on SE; StartCombat and the hit frame open
    with `mov r11/rax, rsp; push rsi; push rdi` (5 bytes), the actor hit with
    `mov rax, rsp; push rdi; push r12` (6 bytes) after the same 9-byte guard,
    and the knock with `mov rax, rsp; push rbp; push rdi` (5 bytes). No branch
    targets the interior of a replaced SE prologue. On SE the hit frame calls
    the actor hit once (+0x38B).
15. [Light budget](#light-budget), 1.7.104: `ShadowSceneNode`'s light update
    (ID 106342, for each light queued by ID 106335) and per-pass dynamic point
    light update (ID 106392) detach a light from every geometry (ID 108285)
    while its `NiLight` has `kHidden` (flags +0xF4 bit 0) or fade (+0x134)
    below 0.05, and light it again once cleared. ID 106392 removes a light whose
    `NiLight` reference count is 1. ID 106335 adds queued lights under
    `lightQueueLock` (+0x1A8). A spell's hand light is
    `TESObjectLIGH::GenDynamic` (ID 17610) on the caster's magic node with
    forced dynamic (ID 34151), stored in `ActorMagicCaster::light`.
16. A torch's light, like the hand light, is an `NiLight` under the actor's 3D,
    whose first `userData` above is the actor, updated every pass as a dynamic
    point light; the engine never app-culls an actor's `NiLight` itself; SE
    treats the flag and fade the same way.
17. [Skeleton pose](#skeleton-pose): Pandora, Nemesis, and FNIS never reorder
    or remove the first 1217 events of the vanilla `0_master` list; a patch
    names its machines and states differently from every vanilla one (a modded
    behavior in a vanilla-named state holds a window only its first second);
    `kIsInKillMove` is set on attacker and victim, and the victim is `kDying`
    from the clip's start.
18. [Solid remote actors](#solid-remote-actors), read from both executables: a
    controller colliding with a remote character body is blocked as by a local
    NPC's, and the warped body does not drift; an arrow's collision with an
    actor pairs its `kProjectile` body with the actor's `kCharController` body
    (`L_PROJECTILE` collides with `L_CHARCONTROLLER` and `L_BIPED_NO_CC`, not
    `L_BIPED`).
19. [Do-nothing procedure target](#do-nothing-procedure-target), 1.7.104: the
    procedure update (ID 39637) picks the procedure per package type, the
    do-nothing type's only entry being ID 39268. It calls
    `DrawWeaponMagicHands(false)` and returns while `WEAPON_STATE` is not
    `kSheathed`; otherwise it resolves `AIProcess::target` (+0x114): empty, it
    runs ID 39799 (sets one only for a package with target data, Sandbox, or
    Patrol) and advances the procedure (ID 39158); set, it requests
    `kActionIdle` (`SetupSpecialIdle`, ID 39256). `DrawWeaponMagicHands`
    (ID 37279) leaves `kWantToSheathe` when the sheathe is refused and turns it
    into `kDrawn` on a draw request. SE is assumed to share this procedure.

## Acceptance criteria

1. Bandits, draugr, and predators never detect, draw on, or attack a remote
   actor. Guards and townsfolk raise no alarm and do not flee because of it.
   Followers do not attack it.
2. A local actor hit by a remote swing does not enter combat with it. A remote
   melee, bash, unarmed attack, or arrow deals no damage, stagger, hit reaction,
   or impulse to anyone or anything, also with Precision installed (`Precision
   interface V3 found`).
3. Combat between local actors and the local player is otherwise unchanged.
4. Hitting a remote actor produces no hit line or grunt; near a fight, crime,
   or sneaking player it speaks no combat, alarm, or detection line, and never
   flees or cowers.
5. Local NPCs still greet, look at, and detect remote actors.
6. Hitting a drawn remote actor does not replay its equip animation; the actor
   stays on the do-nothing package (27) and still uses furniture.
7. A remote actor walking through NPCs, a doorway, or clutter, or idling inside
   an object, moves without shaking, also after a dismount. With
   `DisableCollision` on, the local player walks through it on foot and riding,
   and nothing local is pushed.
8. A swimming remote actor swims (`supported=false`) and leaves swimming when
   its player wades out (`supported=true`), also up a shallow shore.
9. Placement owners log `owner=furniture`, `owner=mount`, `owner=death`, and
   `owner=network` around each interaction; a dying actor ragdolls onto the
   ground. With `DisableCollision` on, a remote actor on a pressure plate does
   not trigger it.
10. With more remote torches or flame spells than `[Advanced] LightsLimit`,
    only that many nearest the camera light their surroundings, the rest still
    show their flames, and players at about the same distance do not trade the
    light. At `0` none lights anything; a freed slot goes to the next light. The
    world's own lights stop flickering.
11. A source gliding with Paraglider and climbing with SkyParkour shows its
    glide and climb on viewers with and without the mods: the actor blends into
    the pose, follows the source's positions without sinking under a climb, and
    blends back after landing. Between windows, checkpoints carry no pose.
12. A modded emote held by the source shows on a viewer who joins during it.
13. With more posed players than `interest.max_posed`, the nearest that many
    show their poses and the others their vanilla reproduction, without
    trading places.
14. Sitting, mounting, dying, and transforming during a window hand the actor
    back to its graph without a lasting wrong pose.
15. A source in first person shows its climb on peers as in third person.
16. A staggered source shows its stagger on peers, and the JellyFish SnowAngel
    power shows on viewers with and without the mod; each window closes with
    `reaction-ended`.
17. A source's kill move shows the source's side on peers; a source killed by a
    kill move ragdolls on peers as soon as it dies.
18. A source knocked down by Unrelenting Force or a giant shows the fall,
    ragdoll, and get-up on peers.
19. With `DisableCollision` on or off, a remote actor's swing, bash, and bow
    shot do nothing locally (the bow launches no arrow), and the local player's
    attacks on it show no damage, sound, blood, stagger, or reaction. With it
    off, the player's arrow stops on the remote actor.
20. With `DisableCollision` off, a standing remote actor blocks the player and
    a walking NPC, stops arrows and spells, and sets off a pressure plate; with
    it on, all pass through. A change applies on each actor's next pass.
21. A remote actor that shouts, then draws and attacks or casts, draws once and
    plays every attack and cast, without `MotionDrivenIdle` blocked every pass
    or repeated `Remote actor weapon state opposes the network` lines.
22. Pushing a remote actor or its mount with Get Out of My Way, or catching it
    in a local explosion, neither staggers nor ragdolls it (`Suppressed knock of
    remote actor`); local NPCs are still pushed.

## Known limitations

- The skeleton pose's COM translation is the source skeleton's, so an actor of
  another height, race, or skeleton stands slightly too high or low during a
  window. A kill move shows only the source's side; a reaction shows the
  source's clip, not a viewer's replacer. A window shows the source's variant
  of a modded animation without its props, sounds, or contrails.
- Weapon hits between the local player and a remote actor show nothing, not
  even impact sound or blood. A remote bow draws and releases but launches no
  arrow.
- [Solid remote actors](#solid-remote-actors) set off local traps, pressure
  plates, and trigger volumes, including quest and scene triggers, and push
  clutter, so with `DisableCollision` off a remote player can change the local
  world. An actor turned solid while overlapping a body collides with it only
  once they separate.
- Skeleton (bone) bodies are not isolated: local spells, melee, traps that test
  them, and the crosshair still find the actor. Isolating them would need the
  bit on every bone body, reapplied after a 3D rebuild and restored on death.
- The light budget counts only remote actors' lights under their 3D, not
  projectiles, impacts, or the local world's lights; a new remote light can show
  for one pass.
- Local actors' non-combat awareness (detection, head tracking, greetings,
  pathing) is not suppressed. Explosions and magic from sources other than the
  actor's own draws and casts are not isolated.
- Hooks considered and not adopted: voice playback (ID 37542), which mutes every
  voice line; package selection (IDs 39114, 38959), which would remove the frame
  a reaction package runs; engine position writes (ID 19790).
