#include "sync/ActorLifecycle.h"
#include "actor/ActorConstants.h"
#include "actor/ActorMount.h"
#include "actor/ActorSpawner.h"
#include "config/PluginConfig.h"
#include "runtime/Profiling.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ActorLifecycle
{
    namespace
    {
        constexpr float kActorSpeed = 550.0F;
        constexpr float kActorStoppingDistance = 1.0F;
        constexpr float kSpawnRetryDelay = 1.0F;
        // A reloaded location normally restores an actor's 3D within a few passes.
        constexpr float kStrandedActorSeconds = 2.0F;
        // Longer than any vanilla mount or dismount clip. Past it the pair is
        // removed anyway rather than kept in the world indefinitely.
        constexpr float kDeferredRemovalTimeoutSeconds = 5.0F;

        // An actor and its mount removed while a mount or dismount clip linked their
        // graphs. Accessed only on the main-loop thread.
        struct DeferredRemoval
        {
            std::string playerID;
            RemoteActor actor;
            std::optional<RemoteActor> mount;
            float elapsed{ 0.0F };
        };

        std::vector<DeferredRemoval> s_deferredRemovals;
        // Spawns the current pass may still start, under the configured rate.
        float s_spawnAllowance{ 0.0F };
        // The longest wait of a spawn held back in the previous pass. A spawn starts
        // only once it has waited as long, so a fixed peer order cannot starve the
        // last peers under sustained demand.
        float s_previousLongestSpawnWait{ 0.0F };
        float s_longestSpawnWait{ 0.0F };

        // a_waitSeconds accumulates while the spawn is held back.
        bool TakeSpawnAllowance(float& a_waitSeconds, float a_delta)
        {
            if (Config::Get().performance.spawnsPerSecond == 0)
            {
                return true;
            }
            if (s_spawnAllowance >= 1.0F && a_waitSeconds >= s_previousLongestSpawnWait)
            {
                s_spawnAllowance -= 1.0F;
                return true;
            }
            a_waitSeconds += std::clamp(a_delta, 0.0F, ActorConstants::kMaximumFrameTime);
            s_longestSpawnWait = (std::max)(s_longestSpawnWait, a_waitSeconds);
            return false;
        }

        // Removes the mount, if any. Called in the same pass as its rider's removal
        // and after it: a seated rider is not taken off first (see ActorMount).
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

        // Removes the actor and its mount, rider first. Removing either while a
        // paired mount clip links their graphs crashes on the next graph event, so
        // such a pair is kept, undriven, until the clip ends.
        void RemoveActorAndMount(const std::string& a_playerID, ActorLifecycleState& a_lifecycle)
        {
            if (!a_lifecycle.actor)
            {
                RemoveMountActor(a_lifecycle);
                return;
            }
            auto* mount = a_lifecycle.mount ? &a_lifecycle.mount->actor : nullptr;
            if (!ActorMount::IsInTransition(*a_lifecycle.actor, mount))
            {
                ActorSpawner::Remove(*a_lifecycle.actor);
                a_lifecycle.actor.reset();
                RemoveMountActor(a_lifecycle);
                return;
            }

            logs::debug(
                "Deferring removal of remote player {} actor {:08X} and mount {:08X} until their mount clip ends",
                a_playerID, a_lifecycle.actor->handle.native_handle(), mount ? mount->handle.native_handle() : 0);
            auto& removal = s_deferredRemovals.emplace_back(
                DeferredRemoval{ .playerID = a_playerID, .actor = std::move(*a_lifecycle.actor) });
            if (mount)
            {
                removal.mount = std::move(*mount);
            }
            a_lifecycle.actor.reset();
            a_lifecycle.mount.reset();
            a_lifecycle.mountSpawnRetryRemaining = 0.0F;
        }

        bool HasDeferredRemoval(const std::string& a_playerID)
        {
            return std::ranges::any_of(
                s_deferredRemovals, [&a_playerID](const DeferredRemoval& a_removal) {
                    return a_removal.playerID == a_playerID;
                });
        }

        // Returns the retired actor's handle for logging.
        std::uint32_t Retire(const RemoteFrame& a_remote, PeerState& a_peer)
        {
            auto& lifecycle = a_peer.lifecycle;
            const auto actorHandle = lifecycle.actor->handle.native_handle();
            RemoveActorAndMount(a_remote.playerID, lifecycle);
            CombatSync::ClearRemoteActions(a_peer.combat);
            MountSync::DiscardRemoteTransition(a_peer.mounting);
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
        const auto actorHandle = Retire(a_remote, a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} after appearance change: spawnedRevision={} revision={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_peer.appearance.spawnedRevision),
            static_cast<unsigned long long>(a_peer.appearance.latest->revision));
    }

    void RetireForSpaceChange(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_localCellID, RE::FormID a_localWorldspaceID)
    {
        const auto actorHandle = Retire(a_remote, a_peer);
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

        const auto actorHandle = Retire(a_remote, a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} after fast travel: reason={} "
            "actorPosition=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) distance={:.1f} loaded3D={} "
            "sequence={}",
            a_remote.playerID, actorHandle, reason, actorPosition.x, actorPosition.y, actorPosition.z, targetPosition.x,
            targetPosition.y, targetPosition.z, distance, has3D,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RetireForRevival(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto actorHandle = Retire(a_remote, a_peer);
        logs::debug("Recycling remote player actor {} actor {:08X} after the player came back to life: sequence={}",
            a_remote.playerID, actorHandle, static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RetireForDismount(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto& mount = *a_peer.lifecycle.mount;
        const auto mountHandle = mount.actor.handle.native_handle();
        const bool transition = ActorMount::IsInTransition(*a_peer.lifecycle.actor, &mount.actor);
        const bool seated = ActorMount::IsSeated(*a_peer.lifecycle.actor, mount.actor);
        const auto actorHandle = Retire(a_remote, a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} and mount {:08X} after the player dismounted: seated={} "
            "transition={} sequence={}",
            a_remote.playerID, actorHandle, mountHandle, seated, transition,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RemoveMountAfterDismount(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto mountHandle = a_peer.lifecycle.mount->actor.handle.native_handle();
        RemoveMountActor(a_peer.lifecycle);
        logs::debug("Removed remote player {} mount {:08X} after actor {:08X} dismounted: sequence={}",
            a_remote.playerID, mountHandle, a_peer.lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RetireForMountLoss(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto mountHandle = a_peer.lifecycle.mount->actor.handle.native_handle();
        const auto actorHandle = Retire(a_remote, a_peer);
        logs::warn("Recycling remote player actor {} actor {:08X} after losing mount {:08X}: sequence={}",
            a_remote.playerID, actorHandle, mountHandle, static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void RetireMountedForDeath(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        const auto& mount = *a_peer.lifecycle.mount;
        const auto mountHandle = mount.actor.handle.native_handle();
        const bool transition = ActorMount::IsInTransition(*a_peer.lifecycle.actor, &mount.actor);
        const auto actorHandle = Retire(a_remote, a_peer);
        logs::debug(
            "Recycling remote player actor {} actor {:08X} and mount {:08X} after the player died riding: "
            "transition={} sequence={}",
            a_remote.playerID, actorHandle, mountHandle, transition,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void YieldFurniture(
        const RemoteFrame& a_remote, PeerState& a_peer, RE::FormID a_furnitureRefID, const char* a_reason)
    {
        auto& lifecycle = a_peer.lifecycle;
        const auto actorHandle = lifecycle.actor ? Retire(a_remote, a_peer) : 0;
        lifecycle.yieldedFurnitureRefID = a_furnitureRefID;
        logs::debug(
            "Hiding remote player actor {} actor {:08X} while furniture {:08X} is in use: reason={} "
            "checkpointFurniture={:08X} sequence={}",
            a_remote.playerID, actorHandle, a_furnitureRefID, a_reason, a_remote.checkpoint.furnitureRefID,
            static_cast<unsigned long long>(a_remote.lastSequence));
    }

    void EndFurnitureYield(const RemoteFrame& a_remote, PeerState& a_peer)
    {
        logs::debug("Restoring remote player actor {} after furniture {:08X} was vacated: sequence={}",
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
        a_peer.lifecycle.spawnRetryRemaining = kSpawnRetryDelay;
    }

    void RemoveActor(const std::string& a_playerID, PeerState& a_peer)
    {
        RemoveActorAndMount(a_playerID, a_peer.lifecycle);
    }

    void UpdateDeferredRemovals(float a_delta)
    {
        PROFILE_FUNCTION();
        if (s_deferredRemovals.empty())
        {
            return;
        }

        const float delta = std::clamp(a_delta, 0.0F, ActorConstants::kMaximumFrameTime);
        std::erase_if(s_deferredRemovals, [delta](DeferredRemoval& a_removal) {
            a_removal.elapsed += delta;
            const auto* mount = a_removal.mount ? &*a_removal.mount : nullptr;
            const bool clipPlaying = ActorMount::IsInTransition(a_removal.actor, mount);
            if (clipPlaying && a_removal.elapsed < kDeferredRemovalTimeoutSeconds)
            {
                return false;
            }
            if (clipPlaying)
            {
                logs::warn("Removing remote player {} actor {:08X} and mount {:08X} while their mount clip still "
                           "plays: waited={:.2f}s",
                    a_removal.playerID, a_removal.actor.handle.native_handle(),
                    mount ? mount->handle.native_handle() : 0, a_removal.elapsed);
            }
            else
            {
                logs::debug(
                    "Removed remote player {} actor {:08X} and mount {:08X} after their mount clip ended: "
                    "waited={:.2f}s",
                    a_removal.playerID, a_removal.actor.handle.native_handle(),
                    mount ? mount->handle.native_handle() : 0, a_removal.elapsed);
            }
            ActorSpawner::Remove(a_removal.actor);
            if (a_removal.mount)
            {
                ActorSpawner::Remove(*a_removal.mount);
            }
            return true;
        });
    }

    void SetDeferredRemovalsExcludedFromSaves(bool a_excluded)
    {
        for (const auto& removal : s_deferredRemovals)
        {
            ActorSpawner::SetExcludedFromSaves(removal.actor, a_excluded);
            if (removal.mount)
            {
                ActorSpawner::SetExcludedFromSaves(*removal.mount, a_excluded);
            }
        }
    }

    void RefillSpawnAllowance(float a_delta)
    {
        const float delta = std::clamp(a_delta, 0.0F, ActorConstants::kMaximumFrameTime);
        const float passShare = static_cast<float>(Config::Get().performance.spawnsPerSecond) * delta;
        // At least one spawn per pass, and never more than one pass's share, so an
        // idle period cannot save up a burst.
        s_spawnAllowance = (std::min)(s_spawnAllowance + passShare, (std::max)(passShare, 1.0F));
        s_previousLongestSpawnWait = std::exchange(s_longestSpawnWait, 0.0F);
    }

    void SpawnWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        PROFILE_FUNCTION();
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
        if (lifecycle.spawnRetryRemaining > 0.0F || HasDeferredRemoval(a_remote.playerID))
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
        if (!TakeSpawnAllowance(lifecycle.spawnAllowanceWaitSeconds, a_delta))
        {
            return;
        }
        const float allowanceWait = std::exchange(lifecycle.spawnAllowanceWaitSeconds, 0.0F);

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

        logs::debug(
            "Allocated remote player actor {} as actor {:08X} at transform sequence={} with target animation "
            "state={}/{} furniture={:08X} spawnAllowanceWait={:.2f}s; waiting for 3D and animation graph readiness",
            a_remote.playerID, lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_peer.appliedSequence),
            Multiplayer::GetMovementStateName(a_remote.checkpoint.animationState.movementState),
            a_remote.checkpoint.animationState.jumping ? "jumping" : "grounded", a_remote.checkpoint.furnitureRefID,
            allowanceWait);

        if (auto* console = RE::ConsoleLog::GetSingleton())
        {
            console->Print("SkyrimMP remote player actor reference allocated; initializing.");
        }
    }

    void SpawnMountWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        lifecycle.mountSpawnRetryRemaining -= (std::max)(a_delta, 0.0F);
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
            mountActor = ActorSpawner::Spawn(mountBase, targetPosition, a_remote.checkpoint.heading,
                Multiplayer::AnimationState{}, kActorSpeed, kActorStoppingDistance);
        }
        if (!mountActor)
        {
            lifecycle.mountSpawnRetryRemaining = kSpawnRetryDelay;
            return;
        }
        // No applied sequence: MountSync hands it the checkpoint's speed before its
        // first gait, which would otherwise start from the spawn speed.
        lifecycle.mount.emplace(MountSync::RemoteMount{ .actor = std::move(*mountActor) });
        logs::debug(
            "Allocated remote player {} mount as actor {:08X} for actor {:08X} at transform sequence={}; waiting "
            "for both to be ready before seating",
            a_remote.playerID, lifecycle.mount->actor.handle.native_handle(), lifecycle.actor->handle.native_handle(),
            static_cast<unsigned long long>(a_peer.lastSequence));
    }
}
