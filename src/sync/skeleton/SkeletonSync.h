#pragma once

#include "protocol/PlayerState.h"
#include "protocol/SkeletonPose.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace SkeletonSync
{
    struct RemoteState
    {
        std::uint64_t fromSequence{ 0 };
        std::uint64_t toSequence{ 0 };
        Network::SkeletonPose from;
        Network::SkeletonPose to;
        // The pose last shown, which a blend-out keeps.
        Network::SkeletonPose shown;
        float weight{ 0.0F };
        // Compared only: the actor whose graph holds the pose.
        const RE::IAnimationGraphManagerHolder* holder{ nullptr };
        // The AssignSlots pass that last placed the peer among the nearest posed peers
        // within the cap; only the latest pass's places count.
        std::uint64_t slotPass{ 0 };
    };

    // Empty unless [Advanced] ShareCustomAnimations is on and a pose window is open.
    [[nodiscard]] Network::EncodedSkeletonPose SampleLocalSkeleton(const LocalFrame& a_local);

    // Shows the peer's pose over the actor's graph while a_owned (the network places the
    // actor on foot in its human form), the played checkpoint carries one, and the peer
    // holds a slot; blends in and out of the graph's own pose.
    void SyncSkeleton(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_owned,
        const RE::NiPoint3& a_viewer, float a_delta);
    void ReleaseSkeleton(RemoteState& a_state, RemoteActor& a_actor);
    // After every peer's SyncSkeleton this pass: gives the slots for the next pass.
    void AssignSlots();
}
