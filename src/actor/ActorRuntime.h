#pragma once

#include "protocol/MovementState.h"

#include <optional>

struct RemoteActor;

namespace ActorMount
{
    struct Gait;
}

namespace ActorRuntime
{
    [[nodiscard]] bool Update(
        RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_translationState);

    [[nodiscard]] bool UpdateRider(
        RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_mountSpeed, bool a_weaponDrawn, float a_deltaTime);

    [[nodiscard]] bool UpdateMount(RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_deltaTime);
}
