#pragma once

#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

#include <optional>

struct RemoteActor;

namespace ActorMovement
{
    struct State
    {
        RE::NiPoint3 targetPosition{};
        RE::NiPoint3 checkpointPosition{};
        float targetHeading{ 0.0F };
        float targetHeadingRate{ 0.0F };
        float targetPitch{ 0.0F };
        float translationSpeed{ 550.0F };
        float animationSpeed{ 550.0F };
        float groundSpeed{ 550.0F };
        float animationDirection{ 0.0F };
        float stoppingDistance{ 1.0F };
        bool sourceSwimming{ false };
        std::optional<RE::NiPoint3> landingPosition;
        float landingApproachElapsed{ 0.0F };
        bool landingApproachEnded{ false };
    };

    void SetTargetMovement(
        RemoteActor& a_remote, const RE::NiPoint3& a_checkpointPosition, float a_speed, bool a_swimming);

    void SetAnimationSample(RemoteActor& a_remote, float a_speed, float a_direction);

    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_headingRate, float a_targetPitch, float a_pathSpeed);

    [[nodiscard]] bool ShouldDeferLanding(RemoteActor& a_remote, bool a_landingPending, float a_deltaTime);

    void ApplyTranslation(
        RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime, Multiplayer::AnimationState a_translationState);

    // Skyrim's movement update derives the pitch from the ground, and the
    // per-frame 3D update builds the body's rotation from it.
    void ApplyPitch(RE::Actor& a_actor, const RemoteActor& a_remote, float a_deltaTime);
}
