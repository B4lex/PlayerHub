#include "actor/ActorMovement.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>

namespace ActorMovement
{
    namespace
    {
        using ActorConstants::kMaximumFrameTime;
        using Multiplayer::IsMoving;

        constexpr float kArrivalTolerance = 1.0F;
        constexpr float kAirborneTransformCatchUpTime = 0.2F;
        // An attack's own motion, such as a forward power attack's lunge, carries the
        // source faster than its last measured speed and on past the end of its
        // movement input. The actor follows it within about this time.
        constexpr float kAttackTransformCatchUpTime = 0.2F;
        // A sample's speed only covers that sample's own travel, so a network stall
        // or a translation hold leaves a backlog that speed matching never recovers
        // while moving, and the idle checkpoint then snaps across it. Drain it over
        // about this time. A steady backlog of one sample merely arrives early.
        constexpr float kLocomotionBacklogDrainTime = 0.5F;
        // Checkpoints are 50 ms apart, so an actor following the network path is
        // normally within one sample of the landing. A larger backlog is snapped by
        // the landing alignment rather than holding the airborne pose longer.
        constexpr float kLandingApproachSeconds = 0.25F;
        // Beyond kArrivalTolerance, where translation stops short of its target.
        constexpr float kLandingArrivalTolerance = 2.0F;
        constexpr float kTwoPi = 6.28318530717958647692F;
        constexpr float kHeadingDeadZone = 0.00872664625997164788F;
        constexpr float kMaximumHeadingRate = kTwoPi;
        // The source's controller already eases its pitch every physics step, so
        // this only has to hide the 50 ms steps between checkpoints.
        constexpr float kPitchSmoothingSeconds = 0.1F;

        void ApplySmoothedHeading(RE::Actor& a_actor, float a_targetHeading, float a_deltaTime)
        {
            if (!std::isfinite(a_targetHeading) || a_deltaTime <= 0.0F)
            {
                return;
            }

            const float currentHeading = a_actor.GetAngleZ();
            if (!std::isfinite(currentHeading))
            {
                a_actor.SetHeading(a_targetHeading);
                return;
            }

            // remainder selects the shortest route across the -pi/pi boundary. Limiting
            // that route keeps individual network samples from snapping the actor and
            // repeatedly provoking Skyrim's native NPC_Turn transitions.
            const float headingDelta = std::remainder(a_targetHeading - currentHeading, kTwoPi);
            if (std::abs(headingDelta) <= kHeadingDeadZone)
            {
                return;
            }

            const float maximumStep = kMaximumHeadingRate * a_deltaTime;
            const float step = std::clamp(headingDelta, -maximumStep, maximumStep);
            a_actor.SetHeading(currentHeading + step);
        }
    }

    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_targetPitch, float a_translationSpeed, float a_direction, bool a_swimming)
    {
        a_remote.movement.targetPosition = a_targetPosition;
        a_remote.movement.targetHeading = a_targetHeading;
        a_remote.movement.targetPitch = a_targetPitch;
        a_remote.movement.sourceSwimming = a_swimming;
        if (std::isfinite(a_translationSpeed) && a_translationSpeed >= 0.0F)
        {
            a_remote.movement.translationSpeed = a_translationSpeed;
            a_remote.movement.animationSpeed = a_translationSpeed;
            a_remote.movement.animationDirection = Multiplayer::NormalizeMovementDirection(a_direction);
            ActorAnimation::RequestAnimationUpdate(a_remote.animation.get(), a_remote.animation->targetAnimationState,
                a_remote.movement.animationSpeed, a_remote.movement.animationDirection);
            ActorRegistry::SetAuthoritativeDirection(a_remote);
        }
    }

    bool ShouldDeferLanding(RemoteActor& a_remote, bool a_landingPending, float a_deltaTime)
    {
        auto& movement = a_remote.movement;
        if (!a_landingPending)
        {
            movement.landingPosition.reset();
            movement.landingApproachEnded = false;
            return false;
        }
        if (movement.landingApproachEnded || !GameThread::Require("ShouldDeferLanding"))
        {
            return false;
        }

        if (movement.landingPosition)
        {
            movement.landingApproachElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        }
        else
        {
            movement.landingPosition = movement.targetPosition;
            movement.landingApproachElapsed = 0.0F;
        }
        const auto actor = a_remote.handle.get();
        if (!actor)
        {
            return false;
        }

        const float distanceToTravel
            = actor->GetPosition().GetDistance(*movement.landingPosition) - movement.stoppingDistance;
        const bool arrived = distanceToTravel <= kLandingArrivalTolerance;
        if (!arrived && movement.landingApproachElapsed < kLandingApproachSeconds)
        {
            return true;
        }
        movement.landingApproachEnded = true;
        if (!arrived)
        {
            // Land on the latest sample instead, as without an approach.
            movement.landingPosition.reset();
        }
        logs::debug("Movement landing approach ended: actor={:08X} result={} elapsed={:.3f} distance={:.1f}",
            a_remote.handle.native_handle(), arrived ? "arrived" : "timed-out", movement.landingApproachElapsed,
            distanceToTravel);
        return false;
    }

    void ApplyTranslation(
        RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime, Multiplayer::AnimationState a_translationState)
    {
        PROFILE_FUNCTION();
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        ApplySmoothedHeading(a_actor, a_remote.movement.targetHeading, deltaTime);

        const auto current = a_actor.GetPosition();
        const auto target = a_remote.movement.landingPosition.value_or(a_remote.movement.targetPosition);

        const RE::NiPoint3 offset{ target.x - current.x, target.y - current.y, target.z - current.z };
        const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

        const float distance = std::sqrt(distanceSquared);
        const float distanceToTravel = distance - a_remote.movement.stoppingDistance;
        if (distanceToTravel <= kArrivalTolerance)
        {
            return;
        }
        const bool attackPlaying = ActorAnimation::IsAnimationActionPlaying(a_remote);
        if (!IsMoving(a_translationState.movementState) && !a_translationState.jumping && !attackPlaying)
        {
            // An idle snapshot is a current-state checkpoint, not an instruction to
            // visibly walk the remaining interpolation backlog while playing idle. A
            // playing attack is not idle: the source's attack motion may still be
            // carrying it after its movement input ended.
            a_actor.SetPosition(target, true);
            return;
        }

        if (deltaTime <= 0.0F)
        {
            return;
        }

        const float catchUpTime = a_translationState.jumping ? kAirborneTransformCatchUpTime
            : attackPlaying                                  ? kAttackTransformCatchUpTime
                                                             : 0.0F;
        // The drain term also applies with a catch-up time, which alone only
        // shortens a backlog above translationSpeed * catchUpTime and would carry
        // a smaller one through the whole jump into the landing.
        const float backlogSpeed = a_remote.movement.translationSpeed + distanceToTravel / kLocomotionBacklogDrainTime;
        const float interpolationSpeed
            = catchUpTime > 0.0F ? (std::max)(backlogSpeed, distanceToTravel / catchUpTime) : backlogSpeed;
        const float step = (std::min)(interpolationSpeed * deltaTime, distanceToTravel);
        const float interpolation = step / distance;

        const RE::NiPoint3 nextPosition{ current.x + offset.x * interpolation, current.y + offset.y * interpolation,
            current.z + offset.z * interpolation };

        // Passing true keeps the actor's character controller in sync with its
        // reference transform, which is essential for a network-controlled remote actor.
        a_actor.SetPosition(nextPosition, true);
    }

    void ApplyPitch(RE::Actor& a_actor, const RemoteActor& a_remote, float a_deltaTime)
    {
        auto* controller = a_actor.GetCharController();
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        // Skyrim pitches only a controller with this flag, which comes from the
        // race's kTiltFrontBack.
        if (!controller || deltaTime <= 0.0F || controller->flags.none(RE::CHARACTER_FLAGS::kCanPitch))
        {
            return;
        }

        const float target = a_remote.movement.targetPitch;
        float& pitch = controller->pitchAngle;
        const float blend = 1.0F - std::exp(-deltaTime / kPitchSmoothingSeconds);
        pitch = std::isfinite(pitch) ? pitch + (target - pitch) * blend : target;
    }
}
