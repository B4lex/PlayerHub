# Furniture Interaction Synchronization

## Status

Implemented for placed `TESFurniture` references on Skyrim AE 1.7.104. Chairs,
benches, alchemy tables, forges, and other workstations share one native entry
path. The engine functions it calls are Address Library IDs verified only
against 1.7.104; on other runtimes the receiver never starts an entry. Crafting
inventory, recipes, menus, skill gains, and produced items are not synchronized.

## Goal

Show a remote player entering, using, and leaving the same world furniture as
the source player, with Skyrim's native entry point, entry idle, furniture idle,
and exit animation.

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
choose all of them locally for the puppet.

Like cell and worldspace identifiers, reference FormIDs require the same plugins
in the same load order on every peer. Old recordings default to
`furnitureRefId: 0`. The server rejects attacks whose prerequisite checkpoint
is using furniture.

## Receiver lifecycle

### Entry

`TESFurniture::Activate` refuses every activator except the player, so the
receiver uses the NPC path. It waits until:

- the reference resolves to a loaded `TESFurniture` in the puppet's coordinate
  space;
- the puppet is ready, its locomotion has settled, and no attack, weapon, or
  graph-recovery transition is pending. A completed weapon clip counts as
  settled while the target is idle (after a sheathe, once the default state has
  been forced), even though its locomotion replay stays armed for later
  movement;
- the puppet is within 24 units of the latest checkpoint;
- the furniture has an available marker (ID 20162), within 96 units (2D) of the
  puppet.

It does not wait for the source to settle on the marker, which would delay the
puppet by the source's whole entry idle. The native entry aligns the puppet to
its chosen entry point.

The receiver then snaps the puppet to the checkpoint transform and calls the
entry used by NPC Sit procedures (ID 37330). It records the occupied
reference, chooses an entry point (ID 39923: entry idle, path point, angle,
marker), and reserves the marker. If no entry point is available, the occupancy
it recorded is cleared and entry is retried after one second.

If the puppet has a weapon drawn, ID 39923 only requests a sheathe and returns
without an entry point (the marker stays unset and no entry idle is chosen).
Skyrim's Sit procedure (ID 39648) selects again on later updates while the
marker is unset, so the receiver repeats the entry once the weapon is sheathed,
retrying every second. If no entry point is chosen within ten seconds, the
occupancy is cleared and a fresh entry is attempted.

The quick entry (ID 37324) is not used: it selects the `Idle*EnterInstant`
records and skips the transition.

### Use

While the puppet occupies furniture, the receiver runs Skyrim's per-frame
sit/sleep state machine (ID 37332 → 39861) itself, because Skyrim only reaches
it from package procedure branches the puppet's do-nothing package never takes.
The state machine plays the entry idle (WantToSit → WaitingForSitAnim), and the
behavior graph completes IsSitting.

While a furniture interaction is active, furniture owns the puppet. Network
translation, heading, locomotion changes, weapon changes, and queued attacks
wait. The animation firewall admits Skyrim's locally generated graph events for
that bounded lifecycle only. Combat, damage, magic, targeting, and survival
policies remain enforced independently.

### Exit

When the checkpoint changes to zero or to a different reference:

- **Seated puppet:** the receiver requests the native exit once per second (get-up
  package, WantToStand/WantToWake, `IdleFurnitureExit`). The state machine
  returns it to Normal.
- **Entry not yet seated:** there is no native exit to request, so the receiver
  clears the occupancy and returns the state to Normal.
- **Completion:** once the puppet is unoccupied and Normal, the receiver restores
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
- Puppet respawn and coordinate-space changes discard local furniture progress
  and reconstruct from the newest checkpoint.
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

1. Entering a resolvable furniture reference makes the remote puppet use that
   same reference with its native entry idle, starting close to the source's
   entry.
2. Leaving it plays the native exit and returns the puppet to the latest
   synchronized locomotion state.
3. Dropped intermediate snapshots and late join converge to the newest occupied
   reference or to no furniture.
4. Furniture use pauses transform interpolation and cannot overlap a
   synchronized attack, jump, or weapon transition.
5. Unresolved, non-furniture, and cross-space references, unavailable markers,
   and stuck transitions are bounded, logged, and retried.
6. Existing recordings without the field replay as `furnitureRefId: 0`.
