# Remote Actor Combat Isolation

## Status

Implemented for Skyrim AE 1.7.104. The hook sites and entry bytes were checked
against the 1.7.104 executable and address library, but the behavior has not yet
been verified in game. Open items are listed under
[Engine assumptions](#engine-assumptions).

## Goal

A remote actor stands in for a player who lives in another world. In the local
world it must be invisible to combat: local enemies, creatures, guards, and
followers never become hostile to it, never target it, and never start combat
with it. The remote actor's replicated attacks do not damage, stagger, or
otherwise hit anything in the local world. Local gameplay continues exactly as
if the actor were not there.

## Non-goals

- Explosions and magic from sources other than a remote actor's own bow draw
  or hand casters. Spec 011 suppresses arrow launches and spell releases for
  the draws and casts it replicates.
- Non-combat awareness: head tracking, greetings, and pathing around the actor.
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
