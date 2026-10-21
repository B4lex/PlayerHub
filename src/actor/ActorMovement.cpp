#include "actor/ActorMovement.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>

namespace ActorMovement
{
    namespace
    {
        using Multiplayer::IsMoving;

        constexpr float kArrivalTolerance = 1.0F;
        constexpr float kMinimumStep = 0.01F;
        constexpr float kAirborneTransformCatchUpSeconds = 0.2F;
        constexpr float kAttackTransformCatchUpSeconds = 0.2F;
        constexpr float kLocomotionBacklogDrainSeconds = 0.5F;
        constexpr float kRestingPathSpeed = 1.0F;
        constexpr float kIdleBacklogDrainSeconds = 0.2F;
        constexpr float kMaximumIdleCatchUpSpeed = 150.0F;
        constexpr float kLandingApproachSeconds = 0.25F;
        constexpr float kLandingArrivalTolerance = 2.0F;
        constexpr float kTwoPi = 6.28318530717958647692F;
        constexpr float kHeadingDeadZone = 0.00872664625997164788F;
        constexpr float kMaximumHeadingRate = kTwoPi;
        constexpr float kRestingHeadingRate = 0.05F;
        constexpr float kHeadingBacklogDrainSeconds = 0.15F;
        constexpr float kPitchSmoothingSeconds = 0.1F;

        void ApplySmoothedHeading(RE::Actor& a_actor, float a_targetHeading, float a_headingRate, float a_deltaTime)
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

            // Snapping the heading provokes Skyrim's native NPC_Turn transitions.
            const float headingDelta = std::remainder(a_targetHeading - currentHeading, kTwoPi);
            const float turnRate = std::isfinite(a_headingRate) ? std::abs(a_headingRate) : 0.0F;
            if (turnRate < kRestingHeadingRate && std::abs(headingDelta) <= kHeadingDeadZone)
            {
                return;
            }

            const float rate
                = (std::min)(turnRate + std::abs(headingDelta) / kHeadingBacklogDrainSeconds, kMaximumHeadingRate);
            const float maximumStep = rate * a_deltaTime;
            const float step = std::clamp(headingDelta, -maximumStep, maximumStep);
            a_actor.SetHeading(currentHeading + step);
        }

        float GetGroundSpeed(float a_speed, const RE::NiPoint3& a_travel)
        {
            const float travelled = a_travel.Length();
            if (travelled <= kMinimumStep)
            {
                return a_speed;
            }
            return a_speed * std::sqrt(a_travel.x * a_travel.x + a_travel.y * a_travel.y) / travelled;
        }
    }

    void SetTargetMovement(
        RemoteActor& a_remote, const RE::NiPoint3& a_checkpointPosition, float a_speed, bool a_swimming)
    {
        const auto travel = a_checkpointPosition - a_remote.movement.checkpointPosition;
        a_remote.movement.checkpointPosition = a_checkpointPosition;
        a_remote.movement.sourceSwimming = a_swimming;
        if (std::isfinite(a_speed) && a_speed >= 0.0F)
        {
            a_remote.movement.groundSpeed = GetGroundSpeed(a_speed, travel);
            ActorAnimation::RequestCheckpointUpdate(a_remote.animation);
        }
    }

    void SetAnimationSample(RemoteActor& a_remote, float a_speed, float a_direction)
    {
        if (!std::isfinite(a_speed) || a_speed < 0.0F)
        {
            return;
        }
        a_remote.movement.animationSpeed = a_speed;
        a_remote.movement.animationDirection = Multiplayer::NormalizeMovementDirection(a_direction);
        ActorRegistry::SetAuthoritativeDirection(a_remote);
    }

    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_headingRate, float a_targetPitch, float a_pathSpeed)
    {
        a_remote.movement.targetPosition = a_targetPosition;
        a_remote.movement.targetHeading = a_targetHeading;
        a_remote.movement.targetHeadingRate = a_headingRate;
        a_remote.movement.targetPitch = a_targetPitch;
        if (std::isfinite(a_pathSpeed) && a_pathSpeed >= 0.0F)
        {
            a_remote.movement.translationSpeed = a_pathSpeed;
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
            movement.landingApproachElapsed += FrameTime::Clamp(a_deltaTime);
        }
        else
        {
            movement.landingPosition = movement.checkpointPosition;
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
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        ApplySmoothedHeading(a_actor, a_remote.movement.targetHeading, a_remote.movement.targetHeadingRate, deltaTime);

        const auto current = a_actor.GetPosition();
        const auto target = a_remote.movement.landingPosition.value_or(a_remote.movement.targetPosition);

        const RE::NiPoint3 offset{ target.x - current.x, target.y - current.y, target.z - current.z };
        const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

        const float distance = std::sqrt(distanceSquared);
        const bool attackPlaying = ActorAnimation::IsAnimationActionPlaying(a_remote);
        if (deltaTime <= 0.0F || distance <= kMinimumStep)
        {
            return;
        }
        if (!IsMoving(a_translationState.movementState) && !a_translationState.jumping && !attackPlaying)
        {
            const float pathSpeed = a_remote.movement.translationSpeed;
            if (pathSpeed < kRestingPathSpeed && distance - a_remote.movement.stoppingDistance <= kArrivalTolerance)
            {
                return;
            }
            const float catchUpSpeed = distance / kIdleBacklogDrainSeconds;
            if (catchUpSpeed > kMaximumIdleCatchUpSpeed)
            {
                a_actor.SetPosition(target, true);
                return;
            }
            const float step = (std::min)((pathSpeed + catchUpSpeed) * deltaTime, distance);
            a_actor.SetPosition(current + offset * (step / distance), true);
            return;
        }

        const float catchUpTime = a_translationState.jumping ? kAirborneTransformCatchUpSeconds
            : attackPlaying                                  ? kAttackTransformCatchUpSeconds
                                                             : 0.0F;
        const float backlogSpeed = a_remote.movement.translationSpeed + distance / kLocomotionBacklogDrainSeconds;
        const float interpolationSpeed
            = catchUpTime > 0.0F ? (std::max)(backlogSpeed, distance / catchUpTime) : backlogSpeed;
        const float step = (std::min)(interpolationSpeed * deltaTime, distance);
        const float interpolation = step / distance;

        const RE::NiPoint3 nextPosition{ current.x + offset.x * interpolation, current.y + offset.y * interpolation,
            current.z + offset.z * interpolation };

        // Passing true keeps the character controller in sync with the reference transform.
        a_actor.SetPosition(nextPosition, true);
    }

    void ApplyPitch(RE::Actor& a_actor, const RemoteActor& a_remote, float a_deltaTime)
    {
        auto* controller = a_actor.GetCharController();
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        // Skyrim pitches only a controller with this flag, which comes from the race's kTiltFrontBack.
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
