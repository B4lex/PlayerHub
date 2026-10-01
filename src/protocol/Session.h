#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Multiplayer
{
    // A session's public identifier: the same for as long as the session lives,
    // whatever its nickname, and never reused. Zero is no session.
    using SessionID = std::uint64_t;

    inline constexpr std::size_t kSessionTokenBytes = 32;

    // A session's secret. Whoever presents it to the server is that player.
    using SessionToken = std::array<std::uint8_t, kSessionTokenBytes>;
}
