#pragma once

#include <cstdint>

namespace Multiplayer
{
    // The server's number for a connected or virtual player. Slots only rise
    // and are never reused within a server run, so a slot names one connection.
    using PeerSlot = std::uint32_t;
}
