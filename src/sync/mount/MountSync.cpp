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
#include <cmath>
#include <cstdint>

namespace MountSync
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;

        constexpr float kSeatRetrySeconds = 0.5F;
        constexpr float kSeatCheckSeconds = 0.5F;
        constexpr float kFlightSampleSeconds = 2.0F;
        constexpr float kRadiansToDegrees = 57.2957795130823208768F;
        constexpr float kTwoPi = 6.28318530717958647692F;

        bool IsFlyingRace(const RE::Actor& a_actor)
        {
            const auto* race = a_actor.GetRace();
            return race && race->data.flags.any(RE::RACE_DATA::Flag::kFlies);
        }

        float GetGraphFloat(RE::Actor& a_actor, const char* a_name)
        {
            float value = 0.0F;
            return a_actor.GetGraphVariableFloat(RE::BSFixedString{ a_name }, value) ? value : -1.0F;
        }

        std::int32_t GetGraphInt(RE::Actor& a_actor, const char* a_name)
        {
            std::int32_t value = -1;
            return a_actor.GetGraphVariableInt(RE::BSFixedString{ a_name }, value) ? value : -1;
        }

        bool GetGraphBool(RE::Actor& a_actor, const char* a_name)
        {
            bool value = false;
            return a_actor.GetGraphVariableBool(RE::BSFixedString{ a_name }, value) && value;
        }

        // The source's own flight graph is what the receiver's flight channels imitate,
        // so its inputs are logged beside the rates the receiver derives.
        void LogLocalFlightSample(
            RE::Actor& a_mount, Multiplayer::FlyState a_state, Multiplayer::WingState a_wing, float a_elapsed)
        {
            struct Sample
            {
                RE::FormID mountID{ 0 };
                float heading{ 0.0F };
                float pitch{ 0.0F };
                RE::NiPoint3 position{};
                float elapsed{ 0.0F };
            };
            static Sample previous{};

            const auto* controller = a_mount.GetCharController();
            const float pitch = controller ? controller->pitchAngle : 0.0F;
            const auto mountID = a_mount.GetFormID();
            if (previous.mountID != mountID)
            {
                previous = Sample{ .mountID = mountID,
                    .heading = a_mount.GetAngleZ(),
                    .pitch = pitch,
                    .position = a_mount.GetPosition() };
                return;
            }
            previous.elapsed += FrameTime::Clamp(a_elapsed);
            if (previous.elapsed < kFlightSampleSeconds)
            {
                return;
            }
            const float elapsed = previous.elapsed;
            const float headingRate = std::remainder(a_mount.GetAngleZ() - previous.heading, kTwoPi) / elapsed;
            const float pitchRate = (pitch - previous.pitch) / elapsed;
            const float speed = a_mount.GetPosition().GetDistance(previous.position) / elapsed;
            logs::debug(
                "Local flying mount sample: mount={:08X} flyState={} wing={} iState={} speed={:.1f} "
                "graphSpeed={:.1f} targetSpeed={:.1f} targetSpeedDamped={:.1f} turnDelta={:.3f} pitchDelta={:.3f} "
                "graphPitch={:.3f} flapping={} gliding={} feathering={} pitch={:.3f} roll={:.3f} canPitch={} "
                "headingRateDegrees={:.1f} pitchRateDegrees={:.1f} angleX={:.3f} moveDirZ={:.3f} "
                "flightPitchBlend={:.3f}",
                mountID, Multiplayer::GetFlyStateName(a_state), Multiplayer::GetWingStateName(a_wing),
                GetGraphInt(a_mount, "iState"), speed, GetGraphFloat(a_mount, "Speed"),
                GetGraphFloat(a_mount, "TargetSpeed"), GetGraphFloat(a_mount, "TargetSpeedDamped"),
                GetGraphFloat(a_mount, "TurnDelta"), GetGraphFloat(a_mount, "PitchDelta"),
                GetGraphFloat(a_mount, "Pitch"), GetGraphBool(a_mount, "IsFlapping"),
                GetGraphBool(a_mount, "IsGliding"), GetGraphBool(a_mount, "IsFeathering"), pitch,
                controller ? controller->rollAngle : 0.0F,
                controller && controller->flags.any(RE::CHARACTER_FLAGS::kCanPitch), headingRate * kRadiansToDegrees,
                pitchRate * kRadiansToDegrees, a_mount.GetAngleX(), GetGraphFloat(a_mount, "MoveDirZ"),
                GetGraphFloat(a_mount, "FlightPitchBlend"));
            previous = Sample{
                .mountID = mountID, .heading = a_mount.GetAngleZ(), .pitch = pitch, .position = a_mount.GetPosition()
            };
        }
    }

    LocalMount SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence, float a_elapsed)
    {
        static RE::FormID previousMountID = 0;
        static auto previousFlyState = Multiplayer::FlyState::kNone;

        LocalMount mount;
        if (!a_local.player.IsOnMount() || !a_local.player.GetMount(mount.actor))
        {
            mount.actor.reset();
        }
        if (mount.actor && IsFlyingRace(*mount.actor))
        {
            mount.flying = true;
            const auto* state = mount.actor->AsActorState();
            const auto flyState = state ? static_cast<std::uint32_t>(state->GetFlyState()) : 0U;
            mount.flyState = flyState <= static_cast<std::uint32_t>(Multiplayer::kMaximumFlyState)
                ? static_cast<Multiplayer::FlyState>(flyState)
                : Multiplayer::FlyState::kNone;
            mount.wingState = ActorFlight::ReadWingState(*mount.actor, mount.flyState);
        }
        const auto mountID = mount.actor ? mount.actor->GetFormID() : 0;
        if (mountID != previousMountID)
        {
            logs::debug("Publishing local player mount: sequence={} mount={:08X} previous={:08X} flying={}",
                static_cast<unsigned long long>(a_sequence), mountID, previousMountID, mount.flying);
            previousMountID = mountID;
        }
        if (mount.flyState != previousFlyState)
        {
            logs::debug("Publishing local flying mount state: sequence={} mount={:08X} flyState={} previous={}",
                static_cast<unsigned long long>(a_sequence), mountID, Multiplayer::GetFlyStateName(mount.flyState),
                Multiplayer::GetFlyStateName(previousFlyState));
            previousFlyState = mount.flyState;
        }
        if (mount.flying)
        {
            LogLocalFlightSample(*mount.actor, mount.flyState, mount.wingState, a_elapsed);
        }
        return mount;
    }

    bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta)
    {
        auto& lifecycle = a_peer.lifecycle;
        // A rider who changes between a horse and a dragon without a dismount between is
        // rebuilt on the new kind, as after a dismount.
        if (!a_remote.checkpoint.mounted || (lifecycle.mount && lifecycle.mount->flying != a_remote.checkpoint.flyingMount))
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
        auto* flight = mount.flying ? &mount.flight : nullptr;
        if (!ActorRuntime::UpdateMount(mount.actor, mount.gait, flight, a_remote.checkpoint.flyState,
                a_remote.checkpoint.wingState, a_delta))
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

        mount.seated = ActorMount::Seat(rider, mount.actor, mount.flying);
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
            rider, mount.riderGait, mount.actor.movement.groundSpeed, mount.flying, weaponDrawn, a_delta);
    }
}
