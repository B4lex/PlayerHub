#include "sync/ActorLifecycle.h"
#include "actor/ActorConstants.h"
#include "actor/ActorSpawner.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>

namespace ActorLifecycle
{
    namespace
    {
        constexpr float kActorSpeed = 550.0F;
        constexpr float kActorStoppingDistance = 1.0F;
        constexpr float kSpawnRetryDelay = 1.0F;
        // A reloaded location normally restores an actor's 3D within a few passes.
        constexpr float kStrandedActorSeconds = 2.0F;

        // Returns the retired actor's handle for logging.
        std::uint32_t Retire(PeerState& a_peer)
        {
            auto& lifecycle = a_peer.lifecycle;
            const auto actorHandle = lifecycle.actor->handle.native_handle();
            ActorSpawner::Remove(*lifecycle.actor);
            lifecycle.actor.reset();
            CombatSync::ClearRemoteActions(a_peer.combat);
            a_peer.appliedSequence = 0;
            lifecycle.spawnRetryRemaining = 0.0F;
            lifecycle.strandedElapsed = 0.0F;
            return actorHandle;
        }
    }

    bool HasChangedSpawnedSpace(const PeerState& a_peer)
    {
        return a_peer.lifecycle.actor
            && !MovementSync::IsSameCoordinateSpace(a_peer.latestCheckpoint.cellID,
                a_peer.latestCheckpoint.worldspaceID, a_peer.lifecycle.spawnedCellID,
                a_peer.lifecycle.spawnedWorldspaceID);
    }

    void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        // Race, sex, head parts, and weight are only applied when 3D loads.
        // Rebuild the actor from the newest checkpoint with the new base.
        const auto actorHandle = Retire(a_peer);
        logs::info(
            "Recycling remote player actor {} actor {:08X} after appearance change: spawnedRevision={} revision={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_peer.appearance.spawnedRevision),
            static_cast<unsigned long long>(a_peer.appearance.latest->revision));
    }

    void RetireForSpaceChange(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID)
    {
        const auto actorHandle = Retire(a_peer);
        logs::info(
            "Recycling remote player actor {} actor {:08X} after coordinate-space change: spawnedCell={:08X} "
            "spawnedWorld={:08X} targetCell={:08X} targetWorld={:08X} localCell={:08X} localWorld={:08X}",
            a_remote.playerID, actorHandle, a_peer.lifecycle.spawnedCellID, a_peer.lifecycle.spawnedWorldspaceID,
            a_remote.checkpoint.cellID, a_remote.checkpoint.worldspaceID, a_localCellID, a_localWorldspaceID);
    }

    void RetireWhenStranded(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        auto actorPtr = lifecycle.actor->handle.get();
        if (!actorPtr)
        {
            // ActorRuntime reports the lost handle.
            return;
        }

        const RE::NiPoint3 targetPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y, a_remote.checkpoint.z };
        const auto actorPosition = actorPtr->GetPosition();
        const float distance = actorPosition.GetDistance(targetPosition);
        const bool has3D = actorPtr->Is3DLoaded();
        const char* reason = nullptr;
        if (distance > MovementSync::kRelocationDistance)
        {
            // Translation would visibly slide across the map, or never happen
            // while the actor's old cell is unloaded.
            reason = "relocated";
        }
        else if (has3D || !ActorSpawner::IsSpawnLocationLoaded(targetPosition))
        {
            lifecycle.strandedElapsed = 0.0F;
        }
        else
        {
            // The actor's own cell stayed unloaded, so it cannot regain 3D, and
            // translation waits for a graph that needs 3D.
            lifecycle.strandedElapsed += std::clamp(a_delta, 0.0F, ActorConstants::kMaximumFrameTime);
            if (lifecycle.strandedElapsed >= kStrandedActorSeconds)
            {
                reason = "unloaded";
            }
        }
        if (!reason)
        {
            return;
        }

        const auto actorHandle = Retire(a_peer);
        logs::info(
            "Recycling remote player actor {} actor {:08X} after fast travel: reason={} "
            "actorPosition=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) distance={:.1f} loaded3D={} "
            "sequence={}",
            a_remote.playerID, actorHandle, reason, actorPosition.x, actorPosition.y, actorPosition.z, targetPosition.x,
            targetPosition.y, targetPosition.z, distance, has3D,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RetireForRevival(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto actorHandle = Retire(a_peer);
        logs::info("Recycling remote player actor {} actor {:08X} after the player came back to life: sequence={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void YieldFurniture(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_furnitureRefID, const char* a_reason)
    {
        auto& lifecycle = a_peer.lifecycle;
        const auto actorHandle = lifecycle.actor ? Retire(a_peer) : 0;
        lifecycle.yieldedFurnitureRefID = a_furnitureRefID;
        logs::info(
            "Hiding remote player actor {} actor {:08X} while furniture {:08X} is in use: reason={} "
            "checkpointFurniture={:08X} sequence={}",
            a_remote.playerID, actorHandle, a_furnitureRefID, a_reason, a_remote.checkpoint.furnitureRefID,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void EndFurnitureYield(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        logs::info("Restoring remote player actor {} after furniture {:08X} was vacated: sequence={}",
            a_remote.playerID, a_peer.lifecycle.yieldedFurnitureRefID,
            static_cast<unsigned long long>(a_remote.lastSequence));
        a_peer.lifecycle.yieldedFurnitureRefID = 0;
    }

    void SetExcludedFromSaves(PeerState& a_peer, bool a_excluded)
    {
        if (a_peer.lifecycle.actor)
        {
            ActorSpawner::SetExcludedFromSaves(*a_peer.lifecycle.actor, a_excluded);
        }
    }

    void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        logs::warn("Remote player actor {} lost its actor handle; respawning it: dead={}", a_remote.playerID,
            a_peer.lifecycle.actor->dead);
        // Only unregisters: the reference is already gone.
        ActorSpawner::Remove(*a_peer.lifecycle.actor);
        a_peer.lifecycle.actor.reset();
        CombatSync::ClearRemoteActions(a_peer.combat);
        a_peer.lifecycle.spawnRetryRemaining = kSpawnRetryDelay;
    }

    void RemoveActor(PeerState& a_peer)
    {
        if (a_peer.lifecycle.actor)
        {
            ActorSpawner::Remove(*a_peer.lifecycle.actor);
        }
    }

    void SpawnWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        if (lifecycle.yieldedFurnitureRefID != 0)
        {
            if (a_remote.checkpoint.furnitureRefID == lifecycle.yieldedFurnitureRefID)
            {
                return;
            }
            lifecycle.yieldedFurnitureRefID = 0;
        }
        lifecycle.spawnRetryRemaining -= (std::max)(a_delta, 0.0F);
        if (lifecycle.spawnRetryRemaining > 0.0F)
        {
            return;
        }

        auto* spawnBase = AppearanceSync::GetSpawnBase(a_peer.appearance, a_remote.playerID);
        if (!spawnBase)
        {
            // Briefly wait for the first appearance so a late joiner is not spawned
            // with the default look and immediately replaced.
            return;
        }

        const RE::NiPoint3 targetPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y, a_remote.checkpoint.z };
        lifecycle.actor = ActorSpawner::Spawn(spawnBase, targetPosition, a_remote.checkpoint.heading,
            Multiplayer::AnimationState{}, kActorSpeed, kActorStoppingDistance);
        if (!lifecycle.actor)
        {
            lifecycle.spawnRetryRemaining = kSpawnRetryDelay;
            return;
        }
        AppearanceSync::OnActorSpawned(a_peer.appearance, spawnBase);
        EquipmentSync::OnActorSpawned(a_peer.equipment);
        FurnitureSync::OnActorSpawned(a_remote, *lifecycle.actor);
        a_peer.appliedSequence = a_peer.lastSequence;
        lifecycle.spawnedCellID = a_remote.checkpoint.cellID;
        lifecycle.spawnedWorldspaceID = a_remote.checkpoint.worldspaceID;

        logs::info(
            "Allocated remote player actor {} as actor {:08X} at transform sequence={} with target animation "
            "state={}/{} furniture={:08X}; waiting for 3D and animation graph readiness",
            a_remote.playerID, lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_peer.appliedSequence),
            Multiplayer::GetMovementStateName(a_remote.checkpoint.animationState.movementState),
            a_remote.checkpoint.animationState.jumping ? "jumping" : "grounded", a_remote.checkpoint.furnitureRefID);

        if (auto* console = RE::ConsoleLog::GetSingleton())
        {
            console->Print("SkyrimMP remote player actor reference allocated; initializing.");
        }
    }
}
