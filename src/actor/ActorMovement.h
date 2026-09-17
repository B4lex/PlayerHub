#pragma once

#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

struct RemoteActor;

namespace ActorMovement
{
    struct State
    {
        RE::NiPoint3 targetPosition{};
        float targetHeading{ 0.0F };
        // Network translation speed is intentionally independent from animation.
        float translationSpeed{ 550.0F };
        // Speed from the same network sample as targetPosition and movement state.
        float animationSpeed{ 550.0F };
        // Graph `Direction` from the same sample: travel relative to heading.
        float animationDirection{ 0.0F };
        float stoppingDistance{ 1.0F };
    };

    void SetTargetTransform(RemoteActor& a_remote, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        float a_translationSpeed, float a_direction);

    // Actor-module internal, called by ActorRuntime once graph state permits
    // translation: smooths heading and moves toward the target transform.
    void ApplyTranslation(
        RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime, Multiplayer::AnimationState a_translationState);
}
