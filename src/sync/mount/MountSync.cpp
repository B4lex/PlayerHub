#include "sync/mount/MountSync.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRuntime.h"
#include "network/SocketIOClient.h"
#include "sync/ActorLifecycle.h"
#include "sync/PeerState.h"
#include "sync/combat/CombatSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <type_traits>

namespace MountSync
{
    namespace
    {
        using ActorConstants::kBlockedEventLogInterval;
        using ActorConstants::kInitialBlockedEventLogs;
        using Network::MountTransition;

        constexpr float kSeatRetrySeconds = 0.5F;
        // Checking the seat looks up the rider's interaction, so it is spread out.
        constexpr float kSeatCheckSeconds = 0.5F;
        // A transition that cannot start this soon after it arrived, for example
        // because the actor or its mount is still loading, is presented instantly
        // instead of late.
        constexpr float kTransitionStartSeconds = 1.0F;
        // The enter is sent right after the checkpoint that starts riding, but may
        // arrive in a later pass. A new mount waits this long for it before seating
        // its rider at once.
        constexpr float kEnterWaitSeconds = 0.25F;
        // A new mount's package and AI process are still being installed around it
        // after it becomes ready, and the first animated seat, started in that
        // pass, ended within half a second.
        constexpr float kAnimatedSeatSettleSeconds = 0.25F;
        // A mount's graph still rejects moveStart in the pass its clip ends, and
        // translating it meanwhile slides it until the gait is corrected.
        constexpr float kClipSettleSeconds = 0.1F;
        // Longer than any vanilla dismount clip. Past it the dismount is treated as
        // failed, and the actor respawns on foot.
        constexpr float kDismountTimeoutSeconds = 5.0F;
        // After its dismount clip, the actor waits beside its mount for the
        // checkpoint that no longer rides. A player still riding after this is
        // seated again.
        constexpr float kDismountedRideSeconds = 2.0F;

        enum class LocalPhase : std::uint8_t
        {
            kOnFoot,
            kEntering,
            kRiding,
            kExiting
        };

        LocalPhase s_localPhase = LocalPhase::kOnFoot;
        // A transition observed by the latest sample, published with its checkpoint.
        std::optional<MountTransition> s_localTransition;

        // The graph's HorseEnter and HorseExit set the sit state of a rider, so it
        // tells the mount and dismount clips apart from an instant seat, which goes
        // straight to riding.
        void ObserveLocalTransition(const LocalFrame& a_local, bool a_mounted, std::uint64_t a_sequence)
        {
            if (!a_mounted)
            {
                s_localPhase = LocalPhase::kOnFoot;
                return;
            }

            // Actor's ActorState base moved on AE; AsActorState relocates it.
            const auto* actorState = a_local.player.AsActorState();
            const auto sitSleepState = actorState ? actorState->GetSitSleepState() : RE::SIT_SLEEP_STATE::kNormal;
            std::optional<MountTransition> transition;
            switch (sitSleepState)
            {
                case RE::SIT_SLEEP_STATE::kWantToSit:
                case RE::SIT_SLEEP_STATE::kWaitingForSitAnim:
                    if (s_localPhase == LocalPhase::kOnFoot)
                    {
                        transition = MountTransition::kEnter;
                        s_localPhase = LocalPhase::kEntering;
                    }
                    break;
                case RE::SIT_SLEEP_STATE::kRidingMount:
                    s_localPhase = LocalPhase::kRiding;
                    break;
                case RE::SIT_SLEEP_STATE::kWantToStand:
                    if (s_localPhase != LocalPhase::kExiting)
                    {
                        transition = MountTransition::kExit;
                        s_localPhase = LocalPhase::kExiting;
                    }
                    break;
                default:
                    break;
            }
            if (transition)
            {
                logs::debug("Observed local player mount transition: sequence={} transition={} sitSleepState={}",
                    static_cast<unsigned long long>(a_sequence), Network::GetMountTransitionName(*transition),
                    static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState));
                s_localTransition = transition;
            }
        }

        bool IsPending(const RemoteState& a_state, MountTransition a_transition)
        {
            return a_state.pending && a_state.pending->transition == a_transition;
        }

        // Plays the dismount until the rider stands beside its mount. Returns whether
        // the mount still owns the actor.
        bool AdvanceDismount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
        {
            auto& mount = *a_peer.lifecycle.mount;
            auto& rider = *a_peer.lifecycle.actor;
            mount.exitElapsed += a_delta;
            if (!mount.transitionPlaying && !ActorMount::IsSeated(rider, mount.actor))
            {
                mount.exiting = false;
                mount.seated = false;
                mount.dismounted = true;
                mount.dismountedElapsed = 0.0F;
                logs::debug(
                    "Remote player {} actor {:08X} finished its dismount from mount {:08X}: elapsed={:.2f}s "
                    "checkpointMounted={} sequence={}",
                    a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(),
                    mount.exitElapsed, a_remote.checkpoint.mounted,
                    static_cast<unsigned long long>(a_remote.lastSequence));
                return true;
            }
            if (mount.exitElapsed < kDismountTimeoutSeconds)
            {
                return true;
            }

            logs::warn("Remote player {} actor {:08X} did not finish its dismount from mount {:08X} in {:.1f}s",
                a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(),
                kDismountTimeoutSeconds);
            ActorLifecycle::RetireForDismount(a_remote, a_peer);
            return false;
        }

        // Starts a received dismount once both actors can play it. Returns whether
        // it started.
        bool StartDismountWhenPending(const RemoteFrame& a_remote, PeerState& a_peer)
        {
            auto& mount = *a_peer.lifecycle.mount;
            auto& rider = *a_peer.lifecycle.actor;
            if (!IsPending(a_peer.mounting, MountTransition::kExit) || !rider.ready || !mount.actor.ready
                || mount.transitionPlaying)
            {
                return false;
            }

            a_peer.mounting.pending.reset();
            const bool started = ActorMount::Dismount(rider);
            logs::debug(
                "Starting remote player {} actor {:08X} dismount from mount {:08X}: started={} checkpointMounted={} "
                "sequence={}",
                a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(), started,
                a_remote.checkpoint.mounted, static_cast<unsigned long long>(a_remote.lastSequence));
            if (started)
            {
                mount.exiting = true;
                mount.exitElapsed = 0.0F;
            }
            return started;
        }

        // Returns whether the mount carries the actor.
        bool SeatWhenDue(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
        {
            auto& mount = *a_peer.lifecycle.mount;
            auto& rider = *a_peer.lifecycle.actor;
            if (mount.seated)
            {
                mount.seatedElapsed += a_delta;
                mount.seatCheckRemaining -= a_delta;
                if (mount.seatCheckRemaining > 0.0F)
                {
                    return true;
                }
                mount.seatCheckRemaining = kSeatCheckSeconds;
                if (ActorMount::IsSeated(rider, mount.actor))
                {
                    return true;
                }
                // Something local took the actor off, or its graph rejected the mount
                // animation; the next attempt seats it again at once.
                mount.seated = false;
                mount.seatRetryRemaining = kSeatRetrySeconds;
                logs::warn(
                    "Remote player {} actor {:08X} left its mount {:08X} locally; seating it again: animated={} "
                    "seatedFor={:.2f}s sitSleepState={} sequence={}",
                    a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(),
                    mount.animatedSeat, mount.seatedElapsed, ActorMount::GetSitSleepState(rider),
                    static_cast<unsigned long long>(a_remote.lastSequence));
            }

            // A transition may still link both graphs through a paired clip.
            const bool canSeat = rider.ready && mount.actor.ready && !mount.transitionPlaying;
            if (canSeat)
            {
                mount.seatRetryRemaining -= a_delta;
            }
            if (!canSeat || mount.seatRetryRemaining > 0.0F)
            {
                return mount.carriesRider;
            }

            // Only a rider standing on foot can play the mount animation.
            const bool animated = !mount.carriesRider && IsPending(a_peer.mounting, MountTransition::kEnter);
            if (animated && mount.actor.readyElapsed < kAnimatedSeatSettleSeconds)
            {
                return false;
            }
            if (!animated && !mount.carriesRider && mount.spawnedElapsed < kEnterWaitSeconds)
            {
                return false;
            }
            if (IsPending(a_peer.mounting, MountTransition::kEnter))
            {
                a_peer.mounting.pending.reset();
            }
            mount.seated = ActorMount::Seat(rider, mount.actor, animated);
            mount.animatedSeat = animated;
            mount.seatedElapsed = 0.0F;
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
            if (mount.seated || seatAttempts <= kInitialBlockedEventLogs
                || seatAttempts % kBlockedEventLogInterval == 0)
            {
                // The seat itself is confirmed by the next seat check.
                logs::debug(
                    "Seating remote player {} actor {:08X} on mount {:08X}: animated={} started={} seatedNow={} "
                    "attempt={} sequence={}",
                    a_remote.playerID, rider.handle.native_handle(), mount.actor.handle.native_handle(), animated,
                    mount.seated, mount.seated && ActorMount::IsSeated(rider, mount.actor), seatAttempts,
                    static_cast<unsigned long long>(a_remote.lastSequence));
            }
            return mount.carriesRider;
        }
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
        ObserveLocalTransition(a_local, static_cast<bool>(mount), a_sequence);
        return mount;
    }

    void PublishLocalTransition(const Network::PlayerTransform& a_checkpoint)
    {
        if (!s_localTransition)
        {
            return;
        }
        Network::PublishLocalMountTransition(*s_localTransition, a_checkpoint.graphEpoch, a_checkpoint.sequence);
        s_localTransition.reset();
    }

    void AcceptRemoteTransition(RemoteState* a_state, const Network::PlayerTransform* a_checkpoint,
        std::uint64_t a_lastSequence, const Network::RemoteMountTransition& a_transition)
    {
        const char* reason = nullptr;
        if (!a_state || !a_checkpoint)
        {
            reason = "unknown player";
        }
        else if (a_transition.graphEpoch != a_checkpoint->graphEpoch)
        {
            reason = "graph epoch";
        }
        else if (a_transition.checkpointSequence > a_lastSequence)
        {
            reason = "future checkpoint";
        }
        else if (a_transition.checkpointSequence <= a_state->latestCheckpointSequence)
        {
            reason = "stale";
        }
        if (reason)
        {
            logs::debug("Mount sync ignored transition: player={} transition={} checkpointSequence={} reason={}",
                a_transition.playerID, Network::GetMountTransitionName(a_transition.transition),
                static_cast<unsigned long long>(a_transition.checkpointSequence), reason);
            return;
        }

        a_state->latestCheckpointSequence = a_transition.checkpointSequence;
        a_state->pending = RemoteState::PendingTransition{ .transition = a_transition.transition,
            .checkpointSequence = a_transition.checkpointSequence };
    }

    void DiscardRemoteTransition(RemoteState& a_state)
    {
        a_state.pending.reset();
    }

    bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        auto& transitions = a_peer.mounting;
        const float delta = std::clamp(a_delta, 0.0F, ActorConstants::kMaximumFrameTime);
        if (transitions.pending)
        {
            transitions.pending->age += delta;
            if (transitions.pending->age > kTransitionStartSeconds)
            {
                logs::debug(
                    "Remote player {} mount transition {} did not start within {:.1f}s; presenting it instantly: "
                    "checkpointSequence={}",
                    a_remote.playerID, Network::GetMountTransitionName(transitions.pending->transition),
                    kTransitionStartSeconds, static_cast<unsigned long long>(transitions.pending->checkpointSequence));
                transitions.pending.reset();
            }
        }

        // A rider waiting for its animated entry, or for a new mount's enter to
        // arrive, stays where the player started it, rather than following the
        // mount's transform on foot.
        const bool enterPending = a_remote.checkpoint.mounted && IsPending(transitions, MountTransition::kEnter);
        if (!lifecycle.mount)
        {
            // DeathSync takes a dying player's actor off its mount before the kill.
            if (a_remote.checkpoint.mounted && !a_remote.checkpoint.dead)
            {
                ActorLifecycle::SpawnMountWhenDue(a_remote, a_peer, a_delta);
            }
            return enterPending;
        }

        auto& mount = *lifecycle.mount;
        auto& rider = *lifecycle.actor;
        if (!a_remote.checkpoint.mounted)
        {
            if (mount.dismounted)
            {
                // The rider already stands on foot, and the dismount clip ended in an
                // earlier pass.
                ActorLifecycle::RemoveMountAfterDismount(a_remote, a_peer);
                ActorMount::ReleaseRider(rider);
                return false;
            }
            if (!mount.exiting && !IsPending(transitions, MountTransition::kExit))
            {
                ActorLifecycle::RetireForDismount(a_remote, a_peer);
                return false;
            }
        }

        mount.spawnedElapsed += delta;
        MovementSync::SyncMovement(a_remote, mount.appliedSequence, mount.actor);
        const bool transitionPlaying = ActorMount::IsInTransition(rider, &mount.actor);
        if (transitionPlaying != mount.transitionPlaying)
        {
            logs::debug(
                "Remote player {} mount clip {}: actor={:08X} mount={:08X} animatedSeat={} exiting={} "
                "seated={} sitSleepState={} sequence={}",
                a_remote.playerID, transitionPlaying ? "started" : "ended", rider.handle.native_handle(),
                mount.actor.handle.native_handle(), mount.animatedSeat, mount.exiting,
                ActorMount::IsSeated(rider, mount.actor), ActorMount::GetSitSleepState(rider),
                static_cast<unsigned long long>(a_remote.lastSequence));
            mount.transitionPlaying = transitionPlaying;
            if (!transitionPlaying)
            {
                mount.clipSettleRemaining = kClipSettleSeconds;
            }
        }
        // Until its rider is first seated, the mount gets no gait: one started from
        // the relocation sample would still run when the mount animation starts.
        auto control = ActorRuntime::MountControl::kDriven;
        if (mount.transitionPlaying || mount.exiting || mount.dismounted || mount.clipSettleRemaining > 0.0F)
        {
            control = ActorRuntime::MountControl::kHeld;
        }
        else if (!mount.carriesRider)
        {
            control = ActorRuntime::MountControl::kPlaced;
        }
        mount.clipSettleRemaining = (std::max)(mount.clipSettleRemaining - delta, 0.0F);
        if (!ActorRuntime::UpdateMount(mount.actor, mount.gait, control, delta))
        {
            ActorLifecycle::RetireForMountLoss(a_remote, a_peer);
            return false;
        }

        if (mount.exiting)
        {
            return AdvanceDismount(a_remote, a_peer, delta);
        }
        if (mount.dismounted)
        {
            mount.dismountedElapsed += delta;
            if (mount.dismountedElapsed >= kDismountedRideSeconds)
            {
                logs::warn(
                    "Remote player {} still rides {:.1f}s after actor {:08X} dismounted; seating it again: "
                    "sequence={}",
                    a_remote.playerID, kDismountedRideSeconds, rider.handle.native_handle(),
                    static_cast<unsigned long long>(a_remote.lastSequence));
                mount.dismounted = false;
                mount.seatRetryRemaining = 0.0F;
            }
            return true;
        }
        if (mount.seated && StartDismountWhenPending(a_remote, a_peer))
        {
            return true;
        }
        if (!a_remote.checkpoint.mounted)
        {
            // The received dismount starts once both actors can play it, or expires.
            return mount.carriesRider;
        }
        const bool awaitingEnter = !mount.carriesRider && mount.spawnedElapsed < kEnterWaitSeconds;
        return SeatWhenDue(a_remote, a_peer, delta) || enterPending || awaitingEnter;
    }

    bool UpdateRider(PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        auto& rider = *lifecycle.actor;
        if (!lifecycle.mount)
        {
            return ActorRuntime::UpdateRider(rider, nullptr, 0.0F, a_delta);
        }

        // During a mount or dismount clip, and beside the mount after one, the
        // rider's graph is not riding.
        auto& mount = *lifecycle.mount;
        const bool riding = mount.seated && !mount.transitionPlaying && !mount.exiting;
        return ActorRuntime::UpdateRider(
            rider, riding ? &mount.riderGait : nullptr, mount.actor.movement.animationSpeed, a_delta);
    }
}
