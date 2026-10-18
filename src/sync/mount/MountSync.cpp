#include "sync/mount/MountSync.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRuntime.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "sync/ActorLifecycle.h"
#include "sync/PeerState.h"
#include "sync/combat/CombatSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>

namespace MountSync
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;

        constexpr float kSeatRetrySeconds = 0.5F;
        constexpr float kSeatCheckSeconds = 0.5F;
    }

    RE::NiPointer<RE::Actor> SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence)
    {
        static RE::FormID previousMountID = 0;

        RE::NiPointer<RE::Actor> mount;
        if (!a_local.player.IsOnMount() || !a_local.player.GetMount(mount))
        {
            mount.reset();
        }
        const auto mountID = mount ? mount->GetFormID() : 0;
        if (mountID != previousMountID)
        {
            logs::debug("Publishing local player mount: sequence={} mount={:08X} previous={:08X}",
                static_cast<unsigned long long>(a_sequence), mountID, previousMountID);
            previousMountID = mountID;
        }
        return mount;
    }

    bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        if (!a_remote.checkpoint.mounted)
        {
            if (lifecycle.mount)
            {
                ActorLifecycle::RetireForDismount(a_remote, a_peer);
            }
            return false;
        }
        if (!lifecycle.mount)
        {
            if (!a_remote.checkpoint.dead)
            {
                ActorLifecycle::SpawnMountWhenDue(a_remote, a_peer, a_delta);
            }
            return false;
        }

        auto& mount = *lifecycle.mount;
        auto& rider = *lifecycle.actor;
        MovementSync::SyncMovement(a_remote, mount.appliedSequence, mount.actor);
        const auto mountBefore = mount.seated ? ActorMount::CapturePlacement(mount.actor) : ActorMount::Placement{};
        if (!ActorRuntime::UpdateMount(mount.actor, mount.gait, a_delta))
        {
            ActorLifecycle::RetireForMountLoss(a_remote, a_peer);
            return false;
        }
        if (mount.seated)
        {
            ActorMount::CarryRider(rider, mount.actor, mountBefore);
        }

        const float delta = FrameTime::Clamp(a_delta);
        if (mount.seated)
        {
            mount.seatCheckRemaining -= delta;
            if (mount.seatCheckRemaining > 0.0F)
            {
                return true;
            }
            mount.seatCheckRemaining = kSeatCheckSeconds;
            if (ActorMount::IsSeated(rider, mount.actor))
            {
                return true;
            }
            // Something local took the actor off; the next attempt seats it again.
            mount.seated = false;
            mount.seatRetryRemaining = kSeatRetrySeconds;
            logs::warn("Remote player {} actor {:08X} left its mount {:08X} locally; seating it again: sequence={}",
                a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(),
                static_cast<unsigned long long>(a_remote.checkpoint.sequence));
        }

        const bool canSeat = rider.ready && mount.actor.ready && !ActorMount::IsInTransition(rider, mount.actor);
        if (canSeat)
        {
            mount.seatRetryRemaining -= delta;
        }
        if (!canSeat || mount.seatRetryRemaining > 0.0F)
        {
            return mount.carriesRider;
        }

        mount.seated = ActorMount::Seat(rider, mount.actor);
        const auto seatAttempts = ++mount.seatAttempts;
        if (mount.seated)
        {
            mount.carriesRider = true;
            mount.seatCheckRemaining = kSeatCheckSeconds;
            // Attacks queued on foot must not play on horseback.
            CombatSync::ClearRemoteActions(a_peer.combat);
        }
        else
        {
            mount.seatRetryRemaining = kSeatRetrySeconds;
        }
        if (mount.seated || ShouldLogOccurrence(seatAttempts))
        {
            logs::debug(
                "Seating remote player {} actor {:08X} on mount {:08X}: started={} seatedNow={} attempt={} "
                "sequence={}",
                a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(), mount.seated,
                mount.seated && ActorMount::IsSeated(rider, mount.actor), seatAttempts,
                static_cast<unsigned long long>(a_remote.checkpoint.sequence));
        }
        return mount.carriesRider;
    }

    bool UpdateRider(const RemoteFrame& a_remote, PeerState& a_peer, bool a_readyForWeaponDraw, float a_delta)
    {
        PROFILE_FUNCTION();
        auto& mount = *a_peer.lifecycle.mount;
        auto& rider = *a_peer.lifecycle.actor;
        // A drawn weapon stays drawn while new hand objects are reconciled, as on foot.
        const bool weaponDrawn = a_remote.checkpoint.animationState.weaponDrawn
            && (a_readyForWeaponDraw || ActorAnimation::GetTargetState(rider).weaponDrawn);
        return ActorRuntime::UpdateRider(
            rider, mount.riderGait, mount.actor.movement.groundSpeed, weaponDrawn, a_delta);
    }
}
