# Remote Actor Combat and Reaction Isolation

## Status

Implemented for Skyrim AE 1.7.104. The hook sites and entry bytes were checked
against the 1.7.104 executable and address library, but the behavior has not yet
been verified in game. Open items are listed under
[Engine assumptions](#engine-assumptions).

[Reaction suppression](#reaction-suppression) adds a second group of hooks that
stop the remote actor's own AI from reacting to the local world, and the
[package takeover](#package-takeover) that goes with them. Both ran in game on
2026-09-20: every hit was met by a blocked dialogue response and by one package
takeover, and the repeating weapon equip animation after a hit was gone.
Sitting in furniture after a hit is the one acceptance criterion the captures
do not cover yet. [Further
candidates](#further-candidates) lists related hooks that are not implemented.

## Goal

A remote actor stands in for a player who lives in another world. In the local
world it must be invisible to combat: local enemies, creatures, guards, and
followers never become hostile to it, never target it, and never start combat
with it. The remote actor's replicated attacks do not damage, stagger, or
otherwise hit anything in the local world. Local gameplay continues exactly as
if the actor were not there.

The remote actor itself does not react to the local world either. Its
presentation comes from the network, so it never speaks barks of its own, such
as hit lines, never flees, and never detects local actors.

## Non-goals

- Explosions and magic from sources other than a remote actor's own bow draw
  or hand casters. Spec 011 suppresses arrow launches and spell releases for
  the draws and casts it replicates.
- Local actors' non-combat awareness of the remote actor: detecting it, head
  tracking, greetings, and pathing around it. Only the remote actor's own
  detection is suppressed.
- Runtimes other than 1.7.104. On other runtimes the hooks are not installed and
  a warning is logged.

## Why enemies attacked actors

The actor previously had `Actor::BOOL_BITS::kPlayerTeammate` set. In 1.7.104:

- The hostility decision (ID 37534, `this` = deciding actor) replaces a target
  that is a player teammate with the player itself before comparing aggression
  and faction reaction. Every actor hostile to the local player was therefore
  hostile to every actor.
- Its wrapper ID 37532, used by `CombatGroup` target handling, returns true for
  a teammate target without consulting the hostility decision at all.

Without the teammate flag the actor still carries the factions copied from the
local player base, and very aggressive actors attack neutral targets, so
removing the flag alone is not enough.

## Design

1. `ApplyRemoteReplicaPolicy` clears `kPlayerTeammate` on every update instead of
   setting it.
2. Three engine functions are hooked at their entry, so every caller sees the
   same answer, including AI worker threads:

   | ID | Function | Moved entry bytes | Actor on either side |
   | --- | --- | --- | --- |
   | 37534 | Hostility decision `(actor, target, aggression, mode)` | `mov [rsp+20h], r9b` | returns false |
   | 37537 | `Actor::IsHostileToActor` (also has its own faction-reaction path) | `mov [rsp+10h], rbx` | returns false |
   | 38561 | `Actor::StartCombat` (also reached from hits, without a hostility check) | `mov r11, rsp; push rbp; push rsi` | returns false |

   The hostility wrappers 37532 (combat group target handling) and 37533 (34
   callers, including `StartCombat`) both tail-call 37534.
3. Each detour copies the first five bytes, which end exactly at an instruction
   boundary and are position independent, into an SKSE trampoline gateway that
   jumps back to the sixth byte, then writes a 5-byte branch to the thunk. The
   bytes are compared first; if they differ, for example because another plugin
   already patched the function, that hook is skipped and an error is logged.
   Startup logs which hooks were installed.
4. A actor is identified by its registration in the actor graph table, looked
   up under the table mutex, so the check is safe from worker threads.
   Registration happens right after the reference is created and is removed
   before the reference is deleted.
5. The frenzy check is inside 37534, which the thunk skips for actors, so
   frenzied local actors also ignore them.
6. Suppressions are logged with the path, both form IDs, and a count, throttled
   like the other firewall logs.
7. Remote actor attack animations play natively, so their `HitFrame`
   annotations still run the local hit pipeline. Two more hooks drop hits whose
   aggressor is a remote actor:

   | ID | Function | Hook | Effect |
   | --- | --- | --- | --- |
   | 38603 | Melee hit frame `(attacker, leftHand, flag)`: weapon reach sweep, then hits on actors (38627) and objects (38628) | entry detour, moved bytes `mov rax, rsp; mov [rax+10h], dl` | returns without sweeping |
   | 44218 + 0x90 | Projectile impact calling the actor hit 38627 `(shooter, target, projectile, leftHand)` | call-site detour | skips the hit |

   The melee hook also covers the queued path: when 38603 defers itself (ID 36915), the deferred task (ID 36991) calls 38603
   again. Skipping 38603 drops damage, stagger, hit impulses, hit effects, and
   the resulting `TESHitEvent` on the target. The entry detour pads its sixth
   byte with a NOP; the call-site detour chains to whatever the call pointed to
   before, so another plugin's hook at that site keeps working.

The existing actor-side suppression (do-nothing package, disabled combat
controller, `SanitizeRemoteCombatState`) and the damage, kill, and magic
firewalls are unchanged.

## Engine assumptions

Verified against the 1.7.104 executable:

1. IDs 37534, 37537, and 38561 resolve to functions with the entry bytes above,
   and those functions take `this` in `rcx` and the target actor in `rdx`.
2. 38561 returns its result in `al` and returns false on its own early rejects.
3. ID 38603 has no return value and takes the attacker in `rcx`. It is the only
   caller of the object hit 38628 and calls the actor hit 38627 twice.
4. ID 38627 takes the aggressor in `rcx`, the target in `rdx`, and an optional
   projectile in `r8`, and has no return value. Its callers are 38603 and 44218,
   which passes the actor resolved from the projectile's shooter handle.

Not yet checked in game:

5. No combat path adds a actor to a local combat group's targets without
   passing through 37532/37534 or `StartCombat`.
6. Clearing `kPlayerTeammate` does not make the local player's hits on a actor
   start a crime or assault reaction. The actor copies the player base, which
   has no crime faction.
7. Bashes and unarmed attacks also land through 38603.

## Acceptance criteria

1. Bandits, draugr, and predators near a actor do not detect it as an enemy,
   draw weapons on it, or attack it, whether or not they are already fighting
   the local player.
2. Guards and townsfolk do not raise an alarm or flee because of a actor.
3. Local followers do not attack a actor.
4. A local actor hit by a actor's replicated swing does not enter combat with
   the actor.
5. A remote actor's melee, bash, or unarmed attack that connects with the local
   player, an NPC, or a physics object deals no damage, causes no stagger or
   hit reaction, and does not move the object.
6. An arrow shot by a remote actor that strikes a local actor deals no damage.
7. Combat between local actors and the local player is otherwise unchanged.

## Reaction suppression

### The old way

A remote actor keeps full actor processing (spec 001), so its AI still
evaluates the local world. Until now its reactions were contained only by
state reasserted from the main loop:

- `ApplyRemoteReplicaPolicy` clears `kCanSpeak`, `kAttackOnSight`, and related
  flags, and disables attacking, on every update.
- `SanitizeRemoteCombatState` runs once per main-loop pass. It stops combat,
  clears the flee flags, stops alarms, restores the do-nothing package when it
  finds a reaction package (flee, alarm, spectator, search, avoid player), and
  resets a fear emotion. It now also takes back any other engine-selected
  package (see "Package takeover").

This is reactive. It removes a reaction after the engine has chosen it, so
the choice can take effect in between, and some paths ignore the flags. In
game, a remote actor that is hit still speaks hit lines even though `kCanSpeak`
is cleared.

### Design

Three engine functions are hooked at their entry, where the reaction is
decided, following the combat hooks above. The approach and two of the IDs come
from TiltedEvolution (`Code/client/Games/Skyrim/Actor.cpp`), which uses AE
Address Library IDs (`versionlib-*.bin`). AE IDs are stable across versions,
and the 1.6.640 and 1.7.104 libraries were compared to confirm the layout
matches.

| ID | Function | Moved entry bytes | Registered remote actor |
| --- | --- | --- | --- |
| 39643 | AI process dialogue response `(process, DialogueItem by value, speaker, listener, flag)` | `mov rax, rsp; mov [rax+10h], rdx` | when the speaker: returns without starting it and releases the item |
| 37577 | `Actor::IsFleeing` | `push rbx; sub rsp, 20h` | returns false |
| 42704 | `ActorKnowledge::SetDetectionState(state)` | `push rbx; sub rsp, 20h` | when the knowledge owner: returns without storing or notifying |

1. 39643 is TiltedEvolution's `ProcessResponse`. TiltedEvolution's signature
   omits the fifth argument, a byte the caller passes on the stack. The thunk
   forwards all five.
2. The dialogue item is passed by value: the caller (ID 39642) takes an extra
   reference and hands 39643 a pointer to that copy, and 39643 releases it
   before returning. The thunk releases it the same way when it blocks,
   through the engine destructor (ID 35221) and sized delete (ID 109588).
   CommonLib's destructor is defaulted and would not free the responses.
   TiltedEvolution's hook skips the release and leaks the item.
3. `IsFleeing` has 22 callers. The thunk still calls the original and logs only
   the calls that would have returned true. `SanitizeRemoteCombatState` still
   clears the flags; the hook keeps callers from acting on them in between.
4. 42704 stores the state at `ActorKnowledge+0x20`, stamps
   `detectionQueuedTimeStamp`, and notifies the listeners when there are any.
   It takes its own reference to the state, so skipping it releases nothing.
   TiltedEvolution blocks only a remote player's detection of the local player.
   Here the remote actor detects no one. The owner is compared by handle
   (`ActorRegistry::IsRegistered(ActorHandle)`) so the hook never resolves a
   handle on an AI thread.
5. The hooks sit in `ReactionSuppression`, separate from `CombatIsolation`,
   because they act on the remote actor's own AI rather than on local actors'
   treatment of it. Both use the shared `CodeDetour` entry detour.
6. Each suppression is logged with its first occurrences and then periodically,
   like the other firewall logs. The dialogue log names the speaker, listener,
   topic, and topic info, so a bark that still plays can be traced.

`ApplyRemoteReplicaPolicy` stays as it was, and `SanitizeRemoteCombatState`
keeps its combat and alarm cleanup (it gains the package takeover below). The
hooks cover the reactions those mechanisms were meant to contain, but each flag
they clear also guards paths the hooks do not touch, so dropping one is its own
change with its own in-game test, not a follow-up to this one.

### Engine assumptions

Verified against the 1.7.104 executable:

1. ID 39643 has one caller, ID 39642 + 0x32C. The caller builds the dialogue
   item for the speaker it holds in `rsi` and passes it in `r8`. 39643 stores
   the item as the process's current dialogue item (`middleHigh+0x360`), has no
   return value, and releases `*rdx` before returning.
2. ID 37577 takes the actor in `rcx` and returns its result in `al`.
3. ID 42704 has one caller. It takes the knowledge in `rcx` and a
   `DetectionState` in `rdx`, and its layout matches CommonLib's
   `ActorKnowledge`.

Observed in game (2026-09-20 capture):

4. Every hit on a remote actor reaches 39643 and is blocked there. The blocked
   responses name topic `00013EBB` and its hit-line infos.

Not yet checked in game:

5. The other barks a remote actor would speak all start through 39642 and
   39643 as well. If any still plays, the voice playback function 37542 is the
   fallback (see below).
6. Suppressing the remote actor's detection has no visible side effect on its
   presentation, such as head tracking that the network does not replicate.

### Package takeover

A hit makes the engine re-evaluate the actor's packages, and it then selects
`DefaultMasterPackage` (form `0003EAB9`, type 18 `kPackage`), which is not a
reaction package, so the reaction-package check above does not catch it. That
package has no reason to hold a drawn weapon, so it asks the actor to sheathe,
while `ApplyAuthoritativeWeaponPresentation` redraws to match the network
state. The two fight once per update until the remote player sheathes, which is
visible as a weapon equip animation that repeats for several seconds.

Observed in the 2026-09-19 capture: a remote actor that was not hit stayed on
the do-nothing package (27) and reached the drawn state in 8 frames, while two
actors that were hit ran package 18 and cycled between `kWantToSheathe` and
`kDrawing` for over 600 frames, ending only when the remote player sheathed.

`SanitizeRemoteCombatState` therefore restores the do-nothing package whenever
the actor is running any other package, not only a reaction package. Its caller
passes `a_mayResetPackage`, which is false until the actor is ready (its first
do-nothing package has not been installed yet) and while a furniture
interaction owns the actor, because furniture entry runs a package of its own.
The sanitization log gained an `unownedPackage` field, and logs the pass that
takes a package back. A takeover also gets its own log line and counter
("Took back local actor package", with the package's type and form), because a
takeover lasts a frame or two and the sanitization counter's logging interval
would otherwise hide it.

This stays reactive: the engine's package still runs for the part of a frame
before the main-loop pass. Candidate 3 below would prevent the selection
instead.

Verified in the 2026-09-20 capture: every one of 21 hits across three actors
was followed on the next main-loop pass, about 15 ms later, by one takeover of
`DefaultMasterPackage`, and never a second one for the same hit. No actor
reported `kWantToSheathe` while the network held the weapon drawn, against many
such frames before the change.

### Acceptance criteria

1. Hitting a remote actor with a weapon, a spell, or a shout produces no hit
   line or grunt from it.
2. A remote actor near a fight, a crime, or a sneaking player speaks no combat,
   alarm, or detection lines.
3. A remote actor never enters a flee or cower reaction, and the combat
   sanitization log reports no flee state for it.
4. Local NPCs still greet, look at, and detect remote actors.
5. The local player's and local NPCs' own dialogue is unchanged.
6. Hitting a remote actor whose weapon is drawn does not make it replay the
   weapon equip animation. The sanitization log reports the do-nothing package
   (27) after the hit, with no alternating `weaponState` of 4 and 2.
7. A remote actor still enters, sits in, and leaves furniture after being hit.

## Further candidates

Not implemented. Each needs its own verification before adoption.

1. **Skipping the remote actor's movement update (ID 37356).**
   TiltedEvolution's `HookActorProcess` returns before this function for every
   remote actor. Despite its name there, it is not AI processing: on 1.7.104
   its only caller is ID 37350 + 0xFB, the call `RemoteCollision` already
   wraps. It is the character movement update: it turns the process velocity
   by the heading, integrates the character controller, and calls
   `SetPosition` (19790) and `SetHeading` (37230) and graph variable setters.
   Remote actors are already placed directly with `SetPosition(..., true)`, so
   skipping it would remove a second writer of their position and heading. It
   would also leave the controller's support and in-air state stale. Swim
   exit (ID 37348), `IsInMidair`, and landing checks read that state, so
   swimming and landing would need network-driven state. Try it behind a
   toggle.
2. **Voice playback (ID 37542, TiltedEvolution's `SpeakSoundFunction`).** Four
   callers. Blocking it for remote actors mutes every voice line, including
   ones outside 39643. It is only needed if criterion 1 fails. Replicated voice
   would need an exception.
3. **Package selection (IDs 39114 `CheckForNewPackage` and 38959
   `InitFromPackage`).** Would keep any package from being chosen instead of
   taking it back afterwards, removing the frame in which the engine's package
   runs (see "Package takeover"). TiltedEvolution compiles both out
   (`#define AI_SYNC 0`), so they are unproven. Our own
   `InitiateDoNothingPackage()`, and the package furniture entry installs, must
   still get through.
4. **Health regeneration (ID 37448).** TiltedEvolution skips health regen for
   remote actors. `SurvivalPolicy` already owns remote health, so this is low
   value.
5. **Engine position writes (ID 19790).** TiltedEvolution ignores
   `SetPosition` on remote actors except its own calls. It belongs with
   candidate 1.

Already covered or not applicable: damage (ID 37335) is handled by the
`HandleHealthDamage` hook, and death items (ID 37198) do not apply because
remote actors never die.
