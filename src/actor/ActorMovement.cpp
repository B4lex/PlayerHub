#include "actor/ActorMovement.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/GroundProbe.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ActorMovement
{
    namespace
    {
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
        constexpr float kGroundProbeAbove = 96.0F;
        constexpr float kGroundProbeBelow = 128.0F;
        constexpr float kGroundProbeIntervalSeconds = 0.1F;
        constexpr float kGroundProbeDistance = 8.0F;
        constexpr float kGroundProbeRefreshSeconds = 1.0F;
        constexpr float kGroundSettleSeconds = 0.25F;
        // A climb's end leaves the source by a ledge lip, where a probe right after the
        // window read the lip 21 to 28 units above it in a replay on the same world.
        constexpr float kPosedGroundSettleSeconds = 0.5F;
        // A source starting a climb presses against the wall, where edge rings read the
        // ledge or the slope below: 30 units off in a replay on the same world.
        constexpr float kPosedEdgeLeadSeconds = 0.5F;
        constexpr float kGroundOffsetSmoothingSeconds = 0.15F;
        constexpr float kGroundOffsetResetDistance = 256.0F;
        constexpr float kGroundOffsetLogChange = 16.0F;
        constexpr std::uint32_t kMaximumGroundRaysPerPass = 24;

        std::uint32_t s_groundRaysThisPass = 0;

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

        RE::NiPoint3 GetSourceTarget(const State& a_movement)
        {
            return a_movement.landingPosition.value_or(a_movement.targetPosition);
        }

        bool IsGroundProbeDue(const GroundOffset& a_ground, const RE::NiPoint3& a_sourcePosition)
        {
            if (!a_ground.probed || a_ground.probeElapsed >= kGroundProbeRefreshSeconds)
            {
                return true;
            }
            const float deltaX = a_sourcePosition.x - a_ground.probePosition.x;
            const float deltaY = a_sourcePosition.y - a_ground.probePosition.y;
            return a_ground.probeElapsed >= kGroundProbeIntervalSeconds
                && deltaX * deltaX + deltaY * deltaY >= kGroundProbeDistance * kGroundProbeDistance;
        }

        void ProbeGround(RE::Actor& a_actor, RemoteActor& a_remote, const RE::NiPoint3& a_sourcePosition)
        {
            auto* cell = a_actor.GetParentCell();
            if (!cell)
            {
                return;
            }
            auto& ground = a_remote.movement.ground;
            const auto probe = GroundProbe::FindSupport(*cell, a_sourcePosition, kGroundProbeAbove, kGroundProbeBelow);
            s_groundRaysThisPass += probe.rays;
            const auto& hit = probe.ground;
            const float target = hit ? hit->z - a_sourcePosition.z : 0.0F;
            if (!ground.probed || hit.has_value() != ground.hit
                || std::abs(target - ground.loggedTarget) >= kGroundOffsetLogChange)
            {
                logs::debug(
                    "Remote actor ground offset target changed: actor={:08X} target={:.1f} previous={:.1f} "
                    "ground={} layer={} edge={} source=({:.1f},{:.1f},{:.1f})",
                    a_remote.handle.native_handle(), target, ground.target, hit.has_value(),
                    hit ? static_cast<int>(hit->layer) : -1, probe.edge, a_sourcePosition.x, a_sourcePosition.y,
                    a_sourcePosition.z);
                ground.loggedTarget = target;
            }
            if (!ground.probed)
            {
                ground.offset = target;
            }
            if (probe.edge)
            {
                if (!ground.edgeRun)
                {
                    ground.edgeRun = true;
                    ground.edgeRunElapsed = 0.0F;
                }
            }
            else
            {
                ground.edgeRun = false;
                ground.plainTarget = target;
            }
            ground.afterPose = false;
            ground.target = target;
            ground.hit = hit.has_value();
            ground.probed = true;
            ground.probeElapsed = 0.0F;
            ground.probePosition = a_sourcePosition;
        }

        void UpdateGroundOffset(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
        {
            auto& movement = a_remote.movement;
            auto& ground = movement.ground;
            ground.probeElapsed += a_deltaTime;
            ground.edgeRunElapsed += a_deltaTime;
            const float settleSeconds = ground.afterPose ? kPosedGroundSettleSeconds : kGroundSettleSeconds;
            if (movement.sourceGrounded)
            {
                ground.groundedElapsed = (std::min)(ground.groundedElapsed + a_deltaTime, settleSeconds);
            }
            // A controller reports kOnGround as soon as its capsule catches a ledge, while its
            // feet are still below it and it climbs on, so a landing is not measured.
            const bool settled = !ground.probed || ground.groundedElapsed >= settleSeconds;
            if (movement.sourceSwimming)
            {
                ground.target = 0.0F;
            }
            else if (movement.sourceGrounded && settled && s_groundRaysThisPass < kMaximumGroundRaysPerPass
                     && IsGroundProbeDue(ground, movement.checkpointPosition))
            {
                // The grounded flag describes the played checkpoint, not the pose
                // interpolated toward the next one, which rises off the ground
                // ahead of a takeoff.
                ProbeGround(a_actor, a_remote, movement.checkpointPosition);
            }
            const float blend = 1.0F - std::exp(-a_deltaTime / kGroundOffsetSmoothingSeconds);
            ground.offset += (ground.target - ground.offset) * blend;
        }
    }

    void BeginPass()
    {
        s_groundRaysThisPass = 0;
    }

    void SetTargetMovement(RemoteActor& a_remote, const RE::NiPoint3& a_checkpointPosition, float a_speed,
        bool a_swimming, bool a_grounded, bool a_posed)
    {
        auto& movement = a_remote.movement;
        auto& ground = movement.ground;
        const auto travel = a_checkpointPosition - movement.checkpointPosition;
        if (travel.Length() > kGroundOffsetResetDistance)
        {
            ground = GroundOffset{};
        }
        if (a_posed && !movement.sourcePosed && ground.edgeRun && ground.edgeRunElapsed < kPosedEdgeLeadSeconds)
        {
            logs::debug("Remote actor ground offset dropped edge probes before a pose: actor={:08X} target={:.1f} "
                        "restored={:.1f} edgeSeconds={:.3f}",
                a_remote.handle.native_handle(), ground.target, ground.plainTarget, ground.edgeRunElapsed);
            ground.target = ground.plainTarget;
            ground.loggedTarget = ground.plainTarget;
            ground.edgeRun = false;
        }
        if (!a_posed && movement.sourcePosed)
        {
            ground.afterPose = true;
        }
        // A posed checkpoint holds the offset like an airborne one.
        const bool grounded = a_grounded && !a_posed;
        if (!grounded)
        {
            ground.groundedElapsed = 0.0F;
        }
        movement.checkpointPosition = a_checkpointPosition;
        movement.sourceSwimming = a_swimming;
        movement.sourceGrounded = grounded;
        movement.sourcePosed = a_posed;
        if (std::isfinite(a_speed) && a_speed >= 0.0F)
        {
            a_remote.movement.groundSpeed = a_speed;
            ActorAnimation::RequestCheckpointUpdate(a_remote.animation);
        }
    }

    void SetPoseWeight(RemoteActor& a_remote, float a_weight)
    {
        a_remote.movement.poseWeight = a_weight;
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

    RE::NiPoint3 GetPlacementTarget(const RemoteActor& a_remote)
    {
        const auto& movement = a_remote.movement;
        auto target = GetSourceTarget(movement);
        target.z += movement.ground.offset;
        return target;
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
            = actor->GetPosition().GetDistance(GetPlacementTarget(a_remote)) - movement.stoppingDistance;
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

    void ApplyTranslation(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime, TranslationMode a_mode)
    {
        PROFILE_FUNCTION();
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        auto& movement = a_remote.movement;
        // A heading snap starts the graph's NPC_Turn transitions, which only a full pose hides.
        if (a_mode == TranslationMode::kPosed && movement.poseWeight >= 1.0F && std::isfinite(movement.targetHeading))
        {
            a_actor.SetHeading(movement.targetHeading);
        }
        else
        {
            ApplySmoothedHeading(a_actor, movement.targetHeading, movement.targetHeadingRate, deltaTime);
        }

        UpdateGroundOffset(a_actor, a_remote, deltaTime);
        if (a_mode == TranslationMode::kPosed)
        {
            // The pose shows where the source's body was at this position; any lag behind it
            // reads as the animation happening beside its place, such as a climb off its wall.
            auto target = movement.targetPosition;
            target.z += movement.ground.offset;
            a_actor.SetPosition(target, true);
            return;
        }
        const auto current = a_actor.GetPosition();
        const auto target = GetPlacementTarget(a_remote);

        const RE::NiPoint3 offset{ target.x - current.x, target.y - current.y, target.z - current.z };
        const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

        const float distance = std::sqrt(distanceSquared);
        if (deltaTime <= 0.0F || distance <= kMinimumStep)
        {
            return;
        }
        if (a_mode == TranslationMode::kStanding)
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

        const float catchUpTime = a_mode == TranslationMode::kAirborne ? kAirborneTransformCatchUpSeconds
            : a_mode == TranslationMode::kAttack                       ? kAttackTransformCatchUpSeconds
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

    void ApplyPitch(RE::Actor& a_actor, const RemoteActor& a_remote, float a_deltaTime, bool a_flying)
    {
        auto* controller = a_actor.GetCharController();
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        // On the ground Skyrim pitches only a controller with this flag, which comes from the
        // race's kTiltFrontBack; the actor's 3D update turns any controller's pitch into the body's.
        if (!controller || deltaTime <= 0.0F || (!a_flying && controller->flags.none(RE::CHARACTER_FLAGS::kCanPitch)))
        {
            return;
        }

        const float target = a_remote.movement.targetPitch;
        float& pitch = controller->pitchAngle;
        const float blend = 1.0F - std::exp(-deltaTime / kPitchSmoothingSeconds);
        pitch = std::isfinite(pitch) ? pitch + (target - pitch) * blend : target;
        // A flying actor's data.angle.x follows its controller's pitch, and Skyrim (ID 39548)
        // builds the graph's MovementDirection from data.angle while it flies: the dragon
        // behavior blends its climb and dive clips by that direction's height.
        if (a_flying)
        {
            a_actor.data.angle.x = pitch;
        }
    }
}
