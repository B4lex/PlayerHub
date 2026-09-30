#pragma once

#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

#include <optional>

struct RemoteActor;

namespace ActorMovement
{
    struct State
    {
        // The interpolated path point, which moves every pass.
        RE::NiPoint3 targetPosition{};
        // Where the played checkpoint was sampled.
        RE::NiPoint3 checkpointPosition{};
        float targetHeading{ 0.0F };
        // Pitch of the source's carrier, applied only where the actor's own
        // controller pitches.
        float targetPitch{ 0.0F };
        // Speed along the interpolated path, intentionally independent from
        // animation.
        float translationSpeed{ 550.0F };
        // Speed from the played checkpoint, with its movement state.
        float animationSpeed{ 550.0F };
        // Graph `Direction` from the same sample: travel relative to heading.
        float animationDirection{ 0.0F };
        float stoppingDistance{ 1.0F };
        // The source swims, so the ground does not support it.
        bool sourceSwimming{ false };
        // Where the source landed. The airborne actor comes down there, rather than
        // cutting toward later samples, before its landing plays.
        std::optional<RE::NiPoint3> landingPosition;
        float landingApproachElapsed{ 0.0F };
        bool landingApproachEnded{ false };
    };

    // Once per played checkpoint: its position, and the speed, direction, and
    // swimming its animation follows.
    void SetTargetMovement(RemoteActor& a_remote, const RE::NiPoint3& a_checkpointPosition, float a_speed,
        float a_direction, bool a_swimming);

    // Every pass: the interpolated path point, heading, and pitch to follow, and
    // the path's speed.
    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_targetPitch, float a_pathSpeed);

    // Holds a grounded checkpoint's landing while the airborne actor travels on to
    // where the source landed, for a bounded time, so the landing does not have to
    // snap the actor across its interpolation backlog.
    [[nodiscard]] bool ShouldDeferLanding(RemoteActor& a_remote, bool a_landingPending, float a_deltaTime);

    // Actor-module internal, called by ActorRuntime once graph state permits
    // translation: smooths heading and moves toward the target transform.
    void ApplyTranslation(
        RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime, Multiplayer::AnimationState a_translationState);

    // Actor-module internal, called by ActorRuntime for a network-placed actor
    // whose race pitches to the ground, such as a mount: eases its controller's
    // pitch toward the target pitch. Skyrim's movement update, which would derive
    // it from the ground, is skipped for such an actor, and its per-frame 3D
    // update builds the body's rotation from it.
    void ApplyPitch(RE::Actor& a_actor, const RemoteActor& a_remote, float a_deltaTime);
}
