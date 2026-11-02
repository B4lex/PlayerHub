#pragma once

#include "protocol/SkeletonPose.h"

#include <RE/Skyrim.h>

#include <cstddef>
#include <optional>

struct RemoteActor;

namespace ActorSkeleton
{
    // Trampoline bytes InstallHooks needs: two call detours.
    inline constexpr std::size_t kTrampolineSize = 2 * 14;

    void InstallHooks();

    // Where the root bone's transform is read relative to.
    enum class RootSpace
    {
        // Its parent bone, as the graph wrote it.
        kParent,
        // The actor's position and heading, from the bone's world transform. A ragdoll
        // carries the whole skeleton with the reference, so only this space shows its tilt.
        kReference
    };

    // The bone rotations Skyrim last wrote to the actor's third-person skeleton.
    [[nodiscard]] std::optional<Network::SkeletonPose> ReadThirdPersonPose(RE::Actor& a_actor, RootSpace a_rootSpace);

    // Blends the graph's output toward the pose on the present bones every animation
    // update until cleared; a_weight 1 replaces it.
    void SetPose(const RemoteActor& a_remote, const Network::SkeletonPose& a_pose, float a_weight);
    void ClearPose(const RemoteActor& a_remote);
}
