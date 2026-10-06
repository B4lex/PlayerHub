#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Multiplayer
{
    using SessionID = std::uint64_t;

    inline constexpr std::size_t kSessionTokenBytes = 32;

    using SessionToken = std::array<std::uint8_t, kSessionTokenBytes>;

    inline constexpr std::size_t kMaximumVanishedSessions = 1024;

    inline constexpr std::size_t kMinimumDisplayNameLength = 3;
    inline constexpr std::size_t kMaximumDisplayNameLength = 15;

    [[nodiscard]] constexpr bool IsValidDisplayName(std::string_view a_name)
    {
        if (a_name.size() < kMinimumDisplayNameLength || a_name.size() > kMaximumDisplayNameLength)
        {
            return false;
        }
        for (const char character : a_name)
        {
            if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9') || character == '_'))
            {
                return false;
            }
        }
        return true;
    }
}
