# Animation Synchronization Analysis

## Current pipeline

```text
Skyrim main-loop call hook (one bound owner thread)
    -> sample transform + locomotion/jump state at 20 Hz
        -> Socket.IO player:update
            -> server validates sequence and relays player:state
                -> network callback keeps newest complete sample per player
                    -> game-thread RemoteActor target
                        -> graph variables/events
                        -> direct transform interpolation
```

The current protocol contains continuous locomotion plus a current jump phase.
It does not yet
capture semantic animation actions such as attacks, furniture, crafting, mounts,
or complex idles, so action replay/epochs are intentionally outside this fix.
The Socket.IO session ID acts as the remote receive-state lifetime boundary.
`player:disconnect` deletes the corresponding actor and clears its receive state;
an interior/worldspace mismatch retires only the actor reference so it can be
recreated at the next compatible snapshot without reusing coordinates from the
old space.

## Proven mismatches

1. Logs show both `PlayerCharacter::UpdateAnimation` and tasks submitted through
   `SKSE::TaskInterface::AddTask` executing on changing worker thread IDs. They
   are not safe ownership boundaries for actor/graph mutations. The complete
   pass now runs from Skyrim's main-loop call site and binds its first observed
   thread ID as the sole owner.
2. The receiver advanced the newest transform while replaying a separate queue
   of older locomotion states. This paired position from one sequence with graph
   state from another. Locomotion now remains latest-wins with its transform.
3. `moveStop` and `SprintStop` were sent after setting `Speed` to zero or
   `IsSprinting` to false. The supplied replay logs show these exact transitions
   rejecting. Stop events now observe old variables; start events observe new
   variables.
4. A graph revision was marked processed after variable writes even when its
   required event returned false. Required events now get four bounded retries.
   The cap is necessary because Skyrim's return value is not a reliable graph
   state acknowledgement and may also mean the transition is already active.
5. The graph used the actor base actor's full walk/run speed while direct
   translation used replicated sample speed. Graph `Speed` and `SpeedSampled`
   now use the same sample as the transform, with actor speed only as a fallback
   for an invalid/zero moving sample. Airborne translation additionally derives
   a catch-up rate from positional error, so zero sampled speed cannot suppress
   vertical correction during a standing jump.
6. Direct translation continued while the graph request was unavailable or
   rejected, which is the literal sliding failure. Translation now waits for the
   matching graph revision and for compound staging to reach the latest
   locomotion state. An idle snapshot snaps the remaining backlog after the idle
   graph state settles rather than visibly translating in idle.
7. Graph writes were queued one task later than the already unsafe frame pass.
   After moving the complete pass to the game thread, graph updates can safely
   run immediately before translation, avoiding an extra frame of locomotion
   lag and avoiding a 20 Hz pause/resume cadence from revision gating.
8. A spawned vanilla NPC remains locally collidable, so attacks can try to
   inject dodge, hit, stagger, or death branches that are absent from the
   network snapshot and steal graph ownership from locomotion. Removing
   collision or clearing `kProcessMe` also removes systems that network movement
   and animation need. The revised policy leaves normal actor processing and
   collision intact, disables attack/cast decisions, and marks the live AI and
   combat-controller paths as ignoring/inactive immediately before every actor
   update. A actor-only graph hook rejects every local animation event while a
   controller-only network dispatch bypasses that public local-event filter.
   A graph output-event sink observes the graph-owned `JumpLandEnd` lifecycle
   marker after the replicated state becomes grounded. The controller waits for
   the observed marker before advancing another
   animation edge, then fully reasserts grounded locomotion. Position updates
   continue during that wait, and locomotion edge changes are held while
   airborne because the jump branch rejects them.
   `bInJumpState` is restored before every jump event, and landing aligns the
   actor with the grounded checkpoint before `JumpLand` is dispatched.
   The same applied jump value is reasserted immediately before and after the
   normal actor update so local controller state cannot overwrite it.
   Some hit reactions enter the behavior graph below that public function, so
   filtering alone is insufficient. Hit events and observed stagger/recoil
   state now schedule a game-thread recovery. The hit callback never mutates
   the actor or graph. The game-thread pass calls Skyrim's native
   `RevertAnimationGraphManager`, invalidates all local dispatch bookkeeping,
   waits for a stable graph/controller, and then replays the newest
   authoritative locomotion state. If the native revert fails or remains
   unavailable for 120 passes, `DoReset3D` reconstructs the graph as a fallback.
9. Every public actor operation that dereferences a Skyrim object checks the
   current Windows thread ID against the thread captured by the main-loop hook.
   `RE::Main::threadID` is not used: the runtime comparison log showed that
   field did not identify the thread executing the frame pass. An accidental
   caller outside the bound thread is rejected and logged once.

## Remaining protocol gaps

- Body heading and graph `Direction` are replicated, so backpedalling and
  strafing use the source graph's directional locomotion. `TurnDelta` is still
  zero, and look/aim direction independent of the body is not replicated.
- There is no timestamped interpolation buffer; transforms chase the newest
  sample. Network jitter can therefore affect spatial smoothness even when
  locomotion state is correct.
- Other stateful semantic actions need a distinct ordered action sequence, graph
  generation/epoch, readiness queue, and reconstruction checkpoint before
  furniture/combat/mount synchronization can be considered reliable.
- Native graph event acceptance is ambiguous. Runtime logs must distinguish
  `processed`, `retrying-events`, `processed-event-timeout`, and
  `variables-rejected` when validating this patch.

## Inspection answers

1. Ordinary locomotion is driven by replicated `movementState` plus sampled
   speed. Jump takeoff begins from `bInJumpState`, while landing is published
   on physical ground contact after an airborne sample. This prevents the
   graph's landing cleanup from extending the replicated airborne phase.
   Remote takeoff and landing events are derived only from changes in that
   applied state.
2. Position, heading, speed, and locomotion state share one snapshot sequence.
   This fix applies graph data before releasing that snapshot's transform. There
   is still no timestamped two-snapshot interpolation timeline.
3. There is no per-actor jump-event queue. Both locomotion and jump phase remain
   latest-wins so a transient event cannot outlive the checkpoint that made it
   valid.
4. Socket.IO provides ordered transport, the server/client reject stale
   snapshot sequences, and the receiver deliberately coalesces locomotion to
   the newest complete sample.
5. Jump phase reuses the snapshot sequence. There is no general animation-action
   sequence distinct from movement or any simulation tick yet.
6. Missing non-jump semantic actions cannot be detected because they are not
   represented by the current protocol.
7. If 3D/graph/variables are unavailable, processed revision resets or remains
   pending and direct translation is held until a later pass succeeds.
8. A locomotion transition is marked processed after graph variables and its
   required events succeed, or after the ambiguous event result reaches the
   bounded four-attempt recovery path.
9. Latest locomotion and jump phase survive receiver setup before actor creation.
   Historical jump actions are not buffered or replayed. Other semantic actions
   are not captured.
10. A lost actor handle discards its local graph bookkeeping and respawns from
    the newest snapshot. Disconnect clears all state for that Socket.IO ID.
11. Late-join furniture, mount, crafting, and complex-idle reconstruction is not
    implemented.
12. Jump and ordinary locomotion are one latest-wins checkpoint. Jump events
    are transient transitions derived at application time.
13. Movement snapshots carry state and speed, but not a general graph-variable
    set or action prerequisites.
14. Hit recovery uses the native graph-manager revert on the game thread, with
    a full 3D reset only as a fallback. A detected graph-manager replacement
    also resets synchronization bookkeeping.
15. No instant reconstruction animation variants are present.
16. Transport ordering survives relay. Application coalescing is intentional
    for both locomotion and jump phase. Future semantic actions still need their
    own sequence and queue.
17. Missing graph variables and rejected/timed-out events are logged. Mod or
    behavior-graph identity mismatch is not explicitly detectable.
18. The supplied logs reproduce movement/graph divergence: actors spawned while
    stale locomotion states replayed, and `SprintStop`/`moveStop` rejected after
    their prerequisite variables had already been cleared.
