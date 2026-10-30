#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"
#include "sync/movement/MovementSync.h"

#include <cstdint>

namespace AnimationSync
{
    struct LocalState;
}

namespace CheckpointPublisher
{
    inline constexpr float kSnapshotIntervalSeconds = 0.05F;

    struct State
    {
        // The first pass after a reset publishes at once.
        float elapsed{ kSnapshotIntervalSeconds };
        float scheduleElapsed{ kSnapshotIntervalSeconds };
        std::uint64_t sequence{ 0 };
        MovementSync::LocalState movement;
    };

    enum class Status
    {
        kNotDue,
        kDisconnected,
        kSkipped,
        kPublished
    };

    struct Publication
    {
        Status status{ Status::kNotDue };
        Network::PlayerTransform checkpoint{};
    };

    [[nodiscard]] Publication PublishLocalCheckpoint(State& a_state, AnimationSync::LocalState& a_animation,
        const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch);
}
