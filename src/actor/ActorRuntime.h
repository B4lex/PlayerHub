#pragma once

#include "protocol/MovementState.h"

#include <cstdint>
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

    // Update of an actor its mount owns: held in place for an animated mount
    // entry, carried while it rides, or standing beside the mount after an
    // animated dismount. Applies the remote-replica policy and, with a_gait, the
    // riding gait of its graph at the mount's speed. Returns false when the actor
    // handle is no longer valid.
    [[nodiscard]] bool UpdateRider(
        RemoteActor& a_remote, ActorMount::Gait* a_gait, float a_mountSpeed, float a_deltaTime);

    enum class MountControl : std::uint8_t
    {
        // Gait and translation follow the network while it carries its rider.
        kDriven,
        // Kept on its target transform without a gait, until its rider is first
        // seated. A new mount beside its rider can be pushed off its spot.
        kPlaced,
        // A mount or dismount clip owns its graph and placement.
        kHeld
    };

    // Update of a remote mount: policy and readiness, then gait and translation
    // toward its target transform as a_control allows. Returns false when the
    // actor handle is no longer valid.
    [[nodiscard]] bool UpdateMount(
        RemoteActor& a_remote, ActorMount::Gait& a_gait, MountControl a_control, float a_deltaTime);
}
