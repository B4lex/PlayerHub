#pragma once

#include "actor/RemoteActor.h"
#include "sync/mount/MountSync.h"

#include <RE/Skyrim.h>

#include <optional>

struct PeerState;
struct RemoteFrame;

struct ActorLifecycleState
{
    std::optional<RemoteActor> actor;
    std::optional<MountSync::RemoteMount> mount;
    float mountSpawnRetryRemaining{ 0.0F };
    float mountSpawnAllowanceWaitSeconds{ 0.0F };
    RE::FormID spawnedCellID{ 0 };
    RE::FormID spawnedWorldspaceID{ 0 };
    float spawnRetryRemaining{ 0.0F };
    float spawnAllowanceWaitSeconds{ 0.0F };
    float strandedElapsed{ 0.0F };
    RE::FormID yieldedFurnitureRefID{ 0 };
};

namespace ActorLifecycle
{
    [[nodiscard]] bool HasChangedSpawnedSpace(const PeerState& a_peer);

    void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireForShapeshift(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireForSpaceChange(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID);
    void RetireWhenStranded(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    void RetireForRevival(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireForDismount(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireForMountLoss(const RemoteFrame& a_remote, PeerState& a_peer);
    void RetireMountedForDeath(const RemoteFrame& a_remote, PeerState& a_peer);
    void YieldFurniture(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_furnitureRefID, const char* a_reason);
    void EndFurnitureYield(const RemoteFrame& a_remote, PeerState& a_peer);
    void SetExcludedFromSaves(PeerState& a_peer, bool a_excluded);
    void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer);
    void RemoveActor(PeerState& a_peer);

    void RefillSpawnAllowance(float a_delta);
    void SpawnWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    void SpawnMountWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
}
