#pragma once

#include <cstdint>

namespace ActorConstants
{
    // Upper bound for one frame's contribution to accumulated elapsed time.
    inline constexpr float kMaximumFrameTime = 0.1F;
    // Repeated hook observations log their first occurrences, then periodically.
    inline constexpr std::uint32_t kInitialBlockedEventLogs = 8;
    inline constexpr std::uint32_t kBlockedEventLogInterval = 120;
}
