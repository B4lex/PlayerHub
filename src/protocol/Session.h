#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Multiplayer
{
    // A session's public identifier: the same for as long as the session lives,
    // whatever its display name, and never reused. Zero is no session.
    using SessionID = std::uint64_t;

    inline constexpr std::size_t kSessionTokenBytes = 32;

    // A session's secret. Whoever presents it to the server is that player.
    using SessionToken = std::array<std::uint8_t, kSessionTokenBytes>;

    // Sessions one client may vanish on a server, as the server accepts.
    inline constexpr std::size_t kMaximumVanishedSessions = 1024;

    inline constexpr std::size_t kMinimumDisplayNameLength = 3;
    inline constexpr std::size_t kMaximumDisplayNameLength = 15;

    // A name a session may take, as the server accepts: 3..15 ASCII letters,
    // digits, and underscores.
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
