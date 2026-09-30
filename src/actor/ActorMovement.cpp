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
        constexpr float kMinimumStep = 0.01F;
        constexpr float kAirborneTransformCatchUpTime = 0.2F;
        // An attack's own motion, such as a forward power attack's lunge, carries the
        // source faster than its last measured speed and on past the end of its
        // movement input. The actor follows it within about this time.
        constexpr float kAttackTransformCatchUpTime = 0.2F;
        // The path speed alone never recovers a distance the actor fell behind the
        // path, such as during a translation hold, and the idle checkpoint would
        // then snap across it. Drain it over about this time.
        constexpr float kLocomotionBacklogDrainTime = 0.5F;
        // An actor following the interpolated path reaches the landing as its
        // checkpoint plays. A larger backlog is snapped by the landing alignment
        // rather than holding the airborne pose longer.
        constexpr float kLandingApproachSeconds = 0.25F;
        // How close to the landing position the airborne actor counts as arrived.
        constexpr float kLandingArrivalTolerance = 2.0F;
        constexpr float kTwoPi = 6.28318530717958647692F;
        constexpr float kHeadingDeadZone = 0.00872664625997164788F;
        constexpr float kMaximumHeadingRate = kTwoPi;
        // The target pitch is already interpolated between checkpoints; this only
        // eases the quantization steps.
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

    void SetTargetMovement(RemoteActor& a_remote, const RE::NiPoint3& a_checkpointPosition, float a_speed,
        float a_direction, bool a_swimming)
    {
        a_remote.movement.checkpointPosition = a_checkpointPosition;
        a_remote.movement.sourceSwimming = a_swimming;
        if (std::isfinite(a_speed) && a_speed >= 0.0F)
        {
            a_remote.movement.animationSpeed = a_speed;
            a_remote.movement.animationDirection = Multiplayer::NormalizeMovementDirection(a_direction);
            ActorAnimation::RequestAnimationUpdate(a_remote.animation.get(), a_remote.animation->targetAnimationState,
                a_remote.movement.animationSpeed, a_remote.movement.animationDirection);
            ActorRegistry::SetAuthoritativeDirection(a_remote);
        }
    }

    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_targetPitch, float a_pathSpeed)
    {
        a_remote.movement.targetPosition = a_targetPosition;
        a_remote.movement.targetHeading = a_targetHeading;
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
            movement.landingApproachElapsed += std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
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
        const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
        ApplySmoothedHeading(a_actor, a_remote.movement.targetHeading, deltaTime);

        const auto current = a_actor.GetPosition();
        const auto target = a_remote.movement.landingPosition.value_or(a_remote.movement.targetPosition);

        const RE::NiPoint3 offset{ target.x - current.x, target.y - current.y, target.z - current.z };
        const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

        const float distance = std::sqrt(distanceSquared);
        const bool attackPlaying = ActorAnimation::IsAnimationActionPlaying(a_remote);
        if (!IsMoving(a_translationState.movementState) && !a_translationState.jumping && !attackPlaying)
        {
            // An idle snapshot is a current-state checkpoint, not an instruction to
            // visibly walk the remaining interpolation backlog while playing idle. A
            // playing attack is not idle: the source's attack motion may still be
            // carrying it after its movement input ended.
            if (distance - a_remote.movement.stoppingDistance > kArrivalTolerance)
            {
                a_actor.SetPosition(target, true);
            }
            return;
        }

        // A moving target advances only a little each pass, less than
        // kArrivalTolerance at a slow walk and a high frame rate, so a moving actor
        // follows it without an arrival tolerance or it would move every other pass.
        if (deltaTime <= 0.0F || distance <= kMinimumStep)
        {
            return;
        }

        const float catchUpTime = a_translationState.jumping ? kAirborneTransformCatchUpTime
            : attackPlaying                                  ? kAttackTransformCatchUpTime
                                                             : 0.0F;
        // The drain term also applies with a catch-up time, which alone only
        // shortens a backlog above translationSpeed * catchUpTime and would carry
        // a smaller one through the whole jump into the landing.
        const float backlogSpeed = a_remote.movement.translationSpeed + distance / kLocomotionBacklogDrainTime;
        const float interpolationSpeed
            = catchUpTime > 0.0F ? (std::max)(backlogSpeed, distance / catchUpTime) : backlogSpeed;
        const float step = (std::min)(interpolationSpeed * deltaTime, distance);
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
