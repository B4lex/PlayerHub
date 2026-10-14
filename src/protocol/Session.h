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

    [[nodiscard]] constexpr bool IsDisplayNameCharacter(char a_character)
    {
        return (a_character >= 'a' && a_character <= 'z') || (a_character >= 'A' && a_character <= 'Z')
            || (a_character >= '0' && a_character <= '9') || a_character == '_';
    }

    [[nodiscard]] constexpr bool IsValidDisplayName(std::string_view a_name)
    {
        if (a_name.size() < kMinimumDisplayNameLength || a_name.size() > kMaximumDisplayNameLength)
        {
            return false;
        }
        for (const char character : a_name)
        {
            if (!IsDisplayNameCharacter(character))
            {
                return false;
            }
        }
        return true;
    }
}
