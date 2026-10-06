#pragma once

#include <cstdint>

namespace ActorConstants
{
    inline constexpr float kMaximumFrameTime = 0.1F;
    inline constexpr std::uint32_t kInitialBlockedEventLogs = 8;
    inline constexpr std::uint32_t kBlockedEventLogInterval = 120;
}
