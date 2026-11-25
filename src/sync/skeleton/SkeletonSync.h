#pragma once

#include "protocol/FacePose.h"
#include "protocol/PlayerState.h"
#include "protocol/SkeletonPose.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

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
        std::optional<Network::FaceWeights> fromFace;
        std::optional<Network::FaceWeights> toFace;
        std::optional<Network::FaceWeights> shownFace;
        float weight{ 0.0F };
        // Compared only: the actor whose graph holds the pose.
        const RE::IAnimationGraphManagerHolder* holder{ nullptr };
    };

    struct LocalPose
    {
        Network::EncodedSkeletonPose skeleton;
        // Only with a skeleton pose.
        Network::EncodedFacePose face;
    };

    // Empty unless a pose window is open. a_voiceMouth: the voice lip sync drives the player's
    // Aah and BigAah phonemes, which viewers derive from the voice themselves.
    [[nodiscard]] LocalPose SampleLocalPose(const LocalFrame& a_local, bool a_voiceMouth);

    // Shows the peer's pose over the actor's graph, and its face over the head's, while a_owned
    // (the network places the actor on foot in its human form) and the played checkpoint
    // carries one; blends in and out of the graph's own pose.
    void SyncSkeleton(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, RE::Actor& a_liveActor,
        bool a_owned, float a_delta);
    void ReleaseSkeleton(RemoteState& a_state, RemoteActor& a_actor);
}
