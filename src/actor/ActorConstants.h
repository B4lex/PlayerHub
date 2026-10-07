#pragma once

#include <cstdint>

namespace ActorConstants
{
    inline constexpr std::uint32_t kInitialBlockedEventLogs = 8;
    inline constexpr std::uint32_t kBlockedEventLogInterval = 120;

    // Counting occurrences rather than time is intended: this only throttles the log.
    constexpr bool ShouldLogOccurrence(std::uint32_t a_count) noexcept
    {
        return a_count <= kInitialBlockedEventLogs || a_count % kBlockedEventLogInterval == 0;
    }
}
