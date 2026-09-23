#pragma once

#include "actor/RemoteActor.h"

#include <RE/Skyrim.h>

#include <optional>

struct PeerState;
struct RemoteFrame;

// Spawn/retire state of a peer's remote actor. The actor handle and its ready
// flag have one owner, RemoteActor; other modules borrow it for a single pass.
struct ActorLifecycleState
{
    std::optional<RemoteActor> actor;
    RE::FormID spawnedCellID{ 0 };
    RE::FormID spawnedWorldspaceID{ 0 };
    float spawnRetryRemaining{ 0.0F };
    // Seconds the actor has been without 3D while its target location is loaded.
    float strandedElapsed{ 0.0F };
    // Furniture the actor is hidden for because the local player or another
    // actor uses it. It is respawned once the newest checkpoint leaves it or the
    // furniture is vacated.
    RE::FormID yieldedFurnitureRefID{ 0 };
};

namespace ActorLifecycle
{
    // The newest checkpoint left the coordinate space the actor was spawned in.
    [[nodiscard]] bool HasChangedSpawnedSpace(const PeerState& a_peer);

    // Retirements keep every reconstructable receive state (checkpoint,
    // appearance, equipment) and drop the transient attack history.
    void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireForSpaceChange(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID);
    // Retires an actor left behind by a fast travel in the same coordinate space:
    // either far from the newest checkpoint, or still without 3D a while after
    // that checkpoint's location has loaded. SpawnWhenDue rebuilds it there.
    void RetireWhenStranded(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    // Retires the actor, if any, and holds its respawn while the newest
    // checkpoint still uses a_furnitureRefID.
    void YieldFurniture(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_furnitureRefID, const char* a_reason);
    // The yielded furniture was vacated; lets SpawnWhenDue respawn the actor.
    void EndFurnitureYield(const RemoteFrame& a_remote, PeerState& a_peer);
    // The actor belongs to the session, not the save. It stays in the world but
    // is excluded while a save is written, then restored once saving finishes.
    void SetExcludedFromSaves(PeerState& a_peer, bool a_excluded);
    // The actor handle became invalid; schedules a respawn after a delay.
    void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer);
    // The player left; the peer state is about to be erased.
    void RemoveOnDisconnect(PeerState& a_peer);

    // Spawns the actor from the newest checkpoint once the retry delay elapsed,
    // an actor base is available, and no yielded furniture holds it back.
    void SpawnWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
}
