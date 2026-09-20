# Furniture Interaction Synchronization

## Status

Implemented for placed `TESFurniture` references on Skyrim AE 1.7.104. Chairs,
benches, alchemy tables, forges, and other workstations share one native entry
path. The engine functions it calls are Address Library IDs verified only
against 1.7.104; on other runtimes the receiver never starts an entry. Crafting
inventory, recipes, menus, skill gains, and produced items are not synchronized.

## Goal

Show a remote player entering, using, and leaving the same world furniture as
the source player, with Skyrim's native entry point, entry idle, and furniture
idle. The exit animation is out of scope; see Exit for why.

## Protocol

Furniture occupancy is reconstructable state, not transient history. Every
`player:update` checkpoint includes:

```json
{
  "furnitureRefId": 42793
}
```

`furnitureRefId` is the 32-bit FormID of the occupied placed reference; zero
means the player is not using furniture. It belongs to the same latest-wins
checkpoint as position, cell/worldspace, locomotion, weapon stance, and graph
epoch. The source samples `Actor::GetOccupiedFurniture()`, which Skyrim sets
when the player activates the furniture, before the player walks to the entry
point and plays the entry idle.

No animation event, idle, or action crosses the wire. The receiver lets Skyrim
choose all of them locally for the actor.

Like cell and worldspace identifiers, reference FormIDs require the same plugins
in the same load order on every peer. Old recordings default to
`furnitureRefId: 0`. The server rejects attacks whose prerequisite checkpoint
is using furniture.

## Receiver lifecycle

### Entry

`TESFurniture::Activate` refuses every activator except the player, so the
receiver uses the NPC path. It waits until:

- the reference resolves to a loaded `TESFurniture` in the actor's coordinate
  space;
- the actor has been ready for 1.5 seconds. An actor created in the pass the
  local cell finished loading has not run one update with its do-nothing
  package, its equipment, or a settled AI process; entry started there is
  accepted, torn down by the engine within one pass of `IsSitting`, and leaves
  the marker reserved;
- the actor is ready, its locomotion has settled, and no attack, weapon, or
  graph-recovery transition is pending. A completed weapon clip counts as
  settled while the target is idle (after a sheathe, once the default state has
  been forced), even though its locomotion replay stays armed for later
  movement;
- the actor is within 24 units of the latest checkpoint;
- the furniture has an available marker (ID 20162), and one of its markers is
  within 96 units (2D) of the actor. The distance is measured to the nearest
  marker, not the one the probe returns: the probe returns the first free
  index while the native entry picks its own entry point, so on a bench the
  probe's marker can lie at the far end while the actor stands beside another
  seat, which held the entry back silently for as long as the source sat;
- the local player does not occupy that same reference. Each peer keeps its own
  world, so both players can be assigned one reference, and the marker probe
  does not report the player's own occupancy. The native entry then records
  occupancy which the engine takes back on the next pass, restarting the entry
  idle for as long as the player stays seated, so nothing is ever played. Two
  actors on one marker is a non-goal; entry waits for the player to leave.

Each of the settled-locomotion, animation-transition, checkpoint-distance and
marker-distance gates logs once when it starts holding a request, with the
distances involved.

It does not wait for the source to settle on the marker, which would delay the
actor by the source's whole entry idle. The native entry aligns the actor to
its chosen entry point.

The receiver then snaps the actor to the checkpoint transform and calls the
entry used by NPC Sit procedures (ID 37330). It records the occupied
reference, chooses an entry point (ID 39923: entry idle, path point, angle,
marker), and reserves the marker. If no entry point is available, the occupancy
it recorded is cleared and entry is retried after one second.

If the actor has a weapon drawn, ID 39923 only requests a sheathe and returns
without an entry point (the marker stays unset and no entry idle is chosen).
Skyrim's Sit procedure (ID 39648) selects again on later updates while the
marker is unset, so the receiver repeats the entry once the weapon is sheathed,
retrying every second. If no entry point is chosen within ten seconds, the
occupancy is cleared and a fresh entry is attempted.

The quick entry (ID 37324) is not used: it selects the `Idle*EnterInstant`
records and skips the transition. Resuming an interaction that began before the
actor existed through it was tried and reverted; it did not reproduce the
source's seated state.

ID 37330 reports success before the occupancy it records is observable, and
sometimes records none at all. The entry is confirmed on a later pass, when the
actor is seen occupying the target. An entry that is never confirmed is retried
after 1.5 seconds rather than after the ten seconds allowed for an entry that
was seated and then dropped.

### Use

While the actor occupies furniture, the receiver runs Skyrim's per-frame
sit/sleep state machine (ID 37332 → 39861) itself, because Skyrim only reaches
it from package procedure branches the actor's do-nothing package never takes.
The state machine plays the entry idle (WantToSit → WaitingForSitAnim), and the
behavior graph completes IsSitting.

The actor stays on its do-nothing package throughout, and any other package
the engine selects is taken back as it is outside furniture (spec 008,
"Package takeover"). A package running during an entry takes the occupancy back
before the next pass. Scene package actions, such as the bard audience sent to
dance while a bard sings, are refused for remote actors (spec 008, "Scene
packages").

While a furniture interaction is active, furniture owns the actor. Network
translation, heading, locomotion changes, weapon changes, and queued attacks
wait. The animation firewall admits Skyrim's locally generated graph events for
that bounded lifecycle only. Combat, damage, magic, targeting, and survival
policies remain enforced independently.

### Exit

When the checkpoint changes to zero or to a different reference:

- **Seated actor:** the receiver sets WantToStand/WantToWake and sends
  `IdleFurnitureExit` on the same pass, which leaves the furniture graph node.
  The state is retried once per second while the actor has not entered one of
  those states, and is not reissued once it has. The actor returns to Normal in
  about 200ms. Any run-once package is released on completion.

  No get-up package is installed. ID 39861 ignores WantToStand, so it drives
  nothing for an actor whose do-nothing package never reaches a procedure, while
  `InitiateGetUpPackage` installs a run-once package whose procedure can never
  finish once the actor has left the furniture. That package outranks the
  do-nothing package and survives it, so combat sanitization retakes it every
  pass for the rest of the session, and the engine moves the actor off its
  marker in between. One capture showed it taken back over 13,000 times,
  starting five milliseconds after the first furniture exit.

  **The exit animation is not synchronized.** ID 37332 -> 39861 in 1.7.104
  dispatches on sit/sleep state and handles only WantToSit, WaitingForSitAnim,
  WantToSleep and WaitingForSleepAnim; WantToStand and WantToWake fall through
  to its epilogue untouched. Entry idles are played from there through ID 40156,
  which is passed `furnitureIdle` at `middleHigh+0x228`. Nothing native drives
  the exit for an actor whose do-nothing package reaches no procedure. Three
  approaches were tried against captures and all failed, none of which should be
  repeated:
  - Deferring `IdleFurnitureExit` so a native get-up could select and play an
    exit idle. `furnitureIdle` stayed on the entry idle for the whole wait;
    39861 shows why nothing is ever selected. The delay also broke exits
    outright, because a source sitting and standing repeatedly cycles faster
    than it, and every return to the furniture restarted the exit.
  - Selecting the exit idle from the entry idle's siblings under its parent and
    playing it through `AIProcess::PlayIdle`. The siblings are not enter/exit
    pairs: parent `0001379F` holds four directional entry variants, so the
    search returned another entry idle for one chair and nothing for the rest.
    `PlayIdle` also refused every call.
  - Holding locomotion after the actor returned to Normal, so a graph-driven
    exit animation would not be overwritten by the dispatch reconstruction.

- **Entry not yet seated:** there is no native exit to request, so the receiver
  releases the actor's marker reservations, clears the occupancy, returns the
  state to Normal, and forces `IdleForceDefaultState`. The event matters: the
  engine bookkeeping is not what is playing the entry idle. Left alone, that
  idle reaches its seated node a second later and the engine sets IsSitting on
  an actor which occupies nothing. Occupancy the actor took on a reference that
  was never requested resolves the same way; if it survives three seconds of
  that, the interaction is abandoned and retried from the entry path.
- **Seated without occupancy:** a sit/sleep state other than Normal is unwound
  even when no occupancy, request, or target is left to key on, through the same
  native exit as a seated actor. Nothing else observes that state, so without it
  the actor holds the pose until something unrelated resets its graph.
- **Completion:** once the actor is unoccupied and Normal, the receiver restores
  the do-nothing package and rebuilds the latest locomotion checkpoint.
- **Stuck exit:** after ten seconds the receiver clears the occupancy, forces
  `IdleForceDefaultState`, and restores the do-nothing package. Target changes
  during an exit do not reset this timer.

Changing directly from one reference to another exits the old furniture before
entering the new one. If Skyrim drops a started entry, ownership is held for at
most ten seconds before the request is released and retried.

## Ordering and recovery

- A furniture checkpoint discards queued attacks and suppresses attack dispatch.
- An already playing attack finishes before furniture entry.
- Furniture exit finishes before newer locomotion or attacks are applied.
- Actor respawn and coordinate-space changes discard local furniture progress
  and reconstruct from the newest checkpoint. A retired actor releases its
  occupancy and its marker reservations first: reservations are keyed by actor
  handle on the furniture reference, and one that outlives its actor makes
  Skyrim refuse every entry point to the replacement.
- Late join reconstructs occupancy from the repeated checkpoint field. Historical
  enter/exit transitions are not replayed.

## Non-goals

- Synchronizing crafting menus, selected recipes, ingredients, output items,
  skill progression, sleeping/waiting UI, or ownership/crime consequences.
- Reserving furniture globally across clients or rendering multiple actors in a
  single-marker furniture object.
- Synchronizing arbitrary activators, doors, containers, scripted furniture,
  paired idles, mounts, or killmoves.
- Translating FormIDs between different plugin load orders.

## Acceptance criteria

1. Entering a resolvable furniture reference makes the remote actor use that
   same reference with its native entry idle, starting close to the source's
   entry.
2. Leaving it plays the native exit and returns the actor to the latest
   synchronized locomotion state.
3. Dropped intermediate snapshots and late join converge to the newest occupied
   reference or to no furniture.
4. Furniture use pauses transform interpolation and cannot overlap a
   synchronized attack, jump, or weapon transition.
5. Unresolved, non-furniture, and cross-space references, unavailable markers,
   and stuck transitions are bounded, logged, and retried.
6. Existing recordings without the field replay as `furnitureRefId: 0`.
