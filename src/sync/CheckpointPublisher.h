#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <cstdint>

namespace CheckpointPublisher
{
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

    [[nodiscard]] Publication PublishLocalCheckpoint(
        const LocalFrame& a_local, float a_delta, std::uint64_t a_graphEpoch);
}
