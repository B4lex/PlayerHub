#pragma once

#include "protocol/MovementState.h"

#include <optional>

struct RemoteActor;

namespace ActorRuntime
{
    // Returns false when the actor handle is no longer valid. a_translationState
    // selects how the transform follows the network path; nullopt holds the
    // transform while a compound transition converges.
    [[nodiscard]] bool Update(
        RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_translationState);
}
