#pragma once

#include <algorithm>
#include <cstdint>

namespace StandIn
{
    // How far a remote player's shown look is from their own.
    enum class Level : std::uint8_t
    {
        kNone,
        kMinor,
        kNotable,
        kMajor
    };

    inline void Raise(Level& a_level, Level a_to)
    {
        a_level = (std::max)(a_level, a_to);
    }
}
