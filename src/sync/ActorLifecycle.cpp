#include "sync/ActorLifecycle.h"
#include "actor/ActorMount.h"
#include "actor/ActorSpawner.h"
#include "config/PluginConfig.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace ActorLifecycle
{
    namespace
    {
        constexpr float kActorSpeed = 550.0F;
        constexpr float kActorStoppingDistance = 1.0F;
        constexpr float kSpawnRetryDelaySeconds = 1.0F;
        constexpr float kStrandedActorSeconds = 2.0F;
        float s_spawnAllowance{ 0.0F };
        std::uint32_t s_spawnsPerSecond{ 0 };
        float s_previousLongestSpawnWait{ 0.0F };
        float s_longestSpawnWait{ 0.0F };

        bool TakeSpawnAllowance(float& a_waitSeconds, float a_delta)
        {
            if (s_spawnsPerSecond == 0)
            {
                return true;
            }
            if (s_spawnAllowance >= 1.0F && a_waitSeconds >= s_previousLongestSpawnWait)
            {
                s_spawnAllowance -= 1.0F;
                return true;
            }
            a_waitSeconds += FrameTime::Clamp(a_delta);
            s_longestSpawnWait = (std::max)(s_longestSpawnWait, a_waitSeconds);
            return false;
        }

        void RemoveMountActor(ActorLifecycleState& a_lifecycle)
        {
            if (!a_lifecycle.mount)
            {
                return;
            }
            ActorSpawner::Remove(a_lifecycle.mount->actor);
            a_lifecycle.mount.reset();
            a_lifecycle.mountSpawnRetryRemaining = 0.0F;
        }

        std::uint32_t Retire(PeerState& a_peer)
        {
            auto& lifecycle = a_peer.lifecycle;
            const auto actorHandle = lifecycle.actor->handle.native_handle();
            ActorSpawner::Remove(*lifecycle.actor);
            lifecycle.actor.reset();
            RemoveMountActor(lifecycle);
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
            && !MovementSync::IsSameCoordinateSpace(a_peer.playout.GetPlayed().cellID,
                a_peer.playout.GetPlayed().worldspaceID, a_peer.lifecycle.spawnedCellID,
                a_peer.lifecycle.spawnedWorldspaceID);
    }

    void RetireForAppearanceChange(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        // Race, sex, head parts, and weight are only applied when 3D loads.
        const auto actorHandle = Retire(a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} after appearance change: spawnedRevision={} revision={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_peer.appearance.spawnedRevision),
            static_cast<unsigned long long>(a_peer.appearance.latest->revision));
    }

    void RetireForSpaceChange(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID)
    {
        const auto actorHandle = Retire(a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} after coordinate-space change: spawnedCell={:08X} "
            "spawnedWorld={:08X} targetCell={:08X} targetWorld={:08X} localCell={:08X} localWorld={:08X}",
            a_remote.playerID, actorHandle, a_peer.lifecycle.spawnedCellID, a_peer.lifecycle.spawnedWorldspaceID,
            a_remote.checkpoint.cellID, a_remote.checkpoint.worldspaceID, a_localCellID, a_localWorldspaceID);
    }

    void RetireWhenStranded(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        PROFILE_FUNCTION();
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
            reason = "relocated";
        }
        else if (has3D || !ActorSpawner::IsSpawnLocationLoaded(targetPosition))
        {
            lifecycle.strandedElapsed = 0.0F;
        }
        else
        {
            lifecycle.strandedElapsed += FrameTime::Clamp(a_delta);
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
        logs::debug(
            "Recycling remote player actor {} actor {:08X} after fast travel: reason={} "
            "actorPosition=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) distance={:.1f} loaded3D={} "
            "sequence={}",
            a_remote.playerID, actorHandle, reason, actorPosition.x, actorPosition.y, actorPosition.z, targetPosition.x,
            targetPosition.y, targetPosition.z, distance, has3D,
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void RetireForRevival(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto actorHandle = Retire(a_peer);
        logs::debug("Recycling remote player actor {} actor {:08X} after the player came back to life: sequence={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void RetireForDismount(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto& mount = *a_peer.lifecycle.mount;
        const auto mountHandle = mount.actor.handle.native_handle();
        const bool transition = ActorMount::IsInTransition(*a_peer.lifecycle.actor, mount.actor);
        const bool seated = ActorMount::IsSeated(*a_peer.lifecycle.actor, mount.actor);
        const auto actorHandle = Retire(a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} and mount {:08X} after the player dismounted: seated={} "
            "transition={} sequence={}",
            a_remote.playerID, actorHandle, mountHandle, seated, transition,
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void RetireForMountLoss(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto mountHandle = a_peer.lifecycle.mount->actor.handle.native_handle();
        const auto actorHandle = Retire(a_peer);
        logs::warn("Recycling remote player actor {} actor {:08X} after losing mount {:08X}: sequence={}",
            a_remote.playerID, actorHandle, mountHandle, static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void RetireMountedForDeath(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto& mount = *a_peer.lifecycle.mount;
        const auto mountHandle = mount.actor.handle.native_handle();
        const bool transition = ActorMount::IsInTransition(*a_peer.lifecycle.actor, mount.actor);
        const auto actorHandle = Retire(a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} and mount {:08X} after the player died riding: "
            "transition={} sequence={}",
            a_remote.playerID, actorHandle, mountHandle, transition,
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void YieldFurniture(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_furnitureRefID, const char* a_reason)
    {
        auto& lifecycle = a_peer.lifecycle;
        const auto actorHandle = lifecycle.actor ? Retire(a_peer) : 0;
        lifecycle.yieldedFurnitureRefID = a_furnitureRefID;
        logs::debug(
            "Hiding remote player actor {} actor {:08X} while furniture {:08X} is in use: reason={} "
            "checkpointFurniture={:08X} sequence={}",
            a_remote.playerID, actorHandle, a_furnitureRefID, a_reason, a_remote.furnitureRefID,
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }

    void EndFurnitureYield(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        logs::debug("Restoring remote player actor {} after furniture {:08X} was vacated: sequence={}",
            a_remote.playerID, a_peer.lifecycle.yieldedFurnitureRefID,
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
        a_peer.lifecycle.yieldedFurnitureRefID = 0;
    }

    void SetExcludedFromSaves(PeerState& a_peer, bool a_excluded)
    {
        if (a_peer.lifecycle.actor)
        {
            ActorSpawner::SetExcludedFromSaves(*a_peer.lifecycle.actor, a_excluded);
        }
        if (a_peer.lifecycle.mount)
        {
            ActorSpawner::SetExcludedFromSaves(a_peer.lifecycle.mount->actor, a_excluded);
        }
    }

    void OnActorLost(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        logs::warn("Remote player actor {} lost its actor handle; respawning it: dead={} mounted={}", a_remote.playerID,
            a_peer.lifecycle.actor->dead, a_peer.lifecycle.mount.has_value());
        // Only unregisters: the reference is already gone.
        ActorSpawner::Remove(*a_peer.lifecycle.actor);
        RemoveMountActor(a_peer.lifecycle);
        a_peer.lifecycle.actor.reset();
        CombatSync::ClearRemoteActions(a_peer.combat);
        a_peer.lifecycle.spawnRetryRemaining = kSpawnRetryDelaySeconds;
    }

    void RemoveActor(PeerState& a_peer)
    {
        if (a_peer.lifecycle.actor)
        {
            ActorSpawner::Remove(*a_peer.lifecycle.actor);
        }
        RemoveMountActor(a_peer.lifecycle);
    }

    void RefillSpawnAllowance(float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        s_spawnsPerSecond = Config::Get()->advanced.spawnsPerSecond;
        const float passShare = static_cast<float>(s_spawnsPerSecond) * delta;
        s_spawnAllowance = (std::min)(s_spawnAllowance + passShare, (std::max)(passShare, 1.0F));
        s_previousLongestSpawnWait = std::exchange(s_longestSpawnWait, 0.0F);
    }

    void SpawnWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        PROFILE_FUNCTION();
        auto& lifecycle = a_peer.lifecycle;
        if (lifecycle.yieldedFurnitureRefID != 0)
        {
            if (a_remote.furnitureRefID == lifecycle.yieldedFurnitureRefID)
            {
                return;
            }
            lifecycle.yieldedFurnitureRefID = 0;
        }
        lifecycle.spawnRetryRemaining -= FrameTime::Clamp(a_delta);
        if (lifecycle.spawnRetryRemaining > 0.0F)
        {
            return;
        }

        auto* spawnBase = AppearanceSync::GetSpawnBase(a_peer.appearance, a_remote.playerID);
        if (!spawnBase)
        {
            // Briefly wait for the first appearance so a late joiner is not spawned with the
            // default look and immediately replaced.
            return;
        }
        if (!TakeSpawnAllowance(lifecycle.spawnAllowanceWaitSeconds, a_delta))
        {
            return;
        }
        const float allowanceWait = std::exchange(lifecycle.spawnAllowanceWaitSeconds, 0.0F);

        const RE::NiPoint3 targetPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y, a_remote.checkpoint.z };
        lifecycle.actor = ActorSpawner::Spawn(
            spawnBase, targetPosition, a_remote.checkpoint.heading, kActorSpeed, kActorStoppingDistance);
        if (!lifecycle.actor)
        {
            lifecycle.spawnRetryRemaining = kSpawnRetryDelaySeconds;
            return;
        }
        AppearanceSync::OnActorSpawned(a_peer.appearance, spawnBase);
        EquipmentSync::OnActorSpawned(a_peer.equipment);
        FurnitureSync::OnActorSpawned(a_remote, *lifecycle.actor);
        a_peer.appliedSequence = a_remote.checkpoint.sequence;
        lifecycle.spawnedCellID = a_remote.checkpoint.cellID;
        lifecycle.spawnedWorldspaceID = a_remote.checkpoint.worldspaceID;

        logs::debug(
            "Allocated remote player actor {} as actor {:08X} at transform sequence={} with target animation "
            "state={}/{} furniture={:08X} spawnAllowanceWait={:.2f}s; waiting for 3D and animation graph readiness",
            a_remote.playerID, lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_peer.appliedSequence),
            Multiplayer::GetMovementStateName(a_remote.checkpoint.animationState.movementState),
            a_remote.checkpoint.animationState.jumping ? "jumping" : "grounded", a_remote.furnitureRefID,
            allowanceWait);
    }

    void SpawnMountWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        lifecycle.mountSpawnRetryRemaining -= FrameTime::Clamp(a_delta);
        if (lifecycle.mountSpawnRetryRemaining > 0.0F
            || !TakeSpawnAllowance(lifecycle.mountSpawnAllowanceWaitSeconds, a_delta))
        {
            return;
        }
        lifecycle.mountSpawnAllowanceWaitSeconds = 0.0F;

        std::optional<RemoteActor> mountActor;
        if (auto* mountBase = ActorMount::GetMountBase())
        {
            const RE::NiPoint3 targetPosition{ a_remote.checkpoint.x, a_remote.checkpoint.y, a_remote.checkpoint.z };
            mountActor = ActorSpawner::Spawn(
                mountBase, targetPosition, a_remote.checkpoint.heading, kActorSpeed, kActorStoppingDistance);
        }
        if (!mountActor)
        {
            lifecycle.mountSpawnRetryRemaining = kSpawnRetryDelaySeconds;
            return;
        }
        // No applied sequence: MountSync hands it the checkpoint's speed before its first gait.
        lifecycle.mount.emplace(MountSync::RemoteMount{ .actor = std::move(*mountActor) });
        logs::debug(
            "Allocated remote player {} mount as actor {:08X} for actor {:08X} at transform sequence={}; waiting "
            "for both to be ready before seating",
            a_remote.playerID, lifecycle.mount->actor.handle.native_handle(), lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_remote.checkpoint.sequence));
    }
}
