#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <cstdint>

// The single local checkpoint publisher. It owns the 20 Hz cadence and the
// sequence, and samples position, heading, speed, direction, animation state,
// and occupied furniture for one coherent player:update.
namespace CheckpointPublisher
{
    enum class Status
    {
        kNotDue,
        kDisconnected,
        // Due, but the player had no parent cell. The interval was consumed.
        kSkipped,
        kPublished
    };

    struct Publication
    {
        Status status{ Status::kNotDue };
        // Valid when status is kPublished.
        Network::PlayerTransform checkpoint{};
    };

    [[nodiscard]] Publication PublishLocalCheckpoint(
        const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch);
}
