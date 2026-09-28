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
    // Returns false when the actor handle is no longer valid. a_translationState
    // selects how the transform follows the network path; nullopt holds the
    // transform while a compound transition converges.
    [[nodiscard]] bool Update(
        RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_translationState);

    // Update of a seated rider, which its mount carries and whose graph the mount
    // interaction owns: the remote-replica policy, and the riding gait of its
    // graph at the mount's speed. Returns false when the actor handle is no longer
    // valid.
    [[nodiscard]] bool UpdateRider(
        RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_mountSpeed, float a_deltaTime);

    // Update of a remote mount: policy, readiness, gait, and translation toward
    // its target transform. Returns false when the actor handle is no longer valid.
    [[nodiscard]] bool UpdateMount(RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_deltaTime);
}
