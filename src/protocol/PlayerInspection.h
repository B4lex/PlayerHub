#pragma once

#include "protocol/FormReference.h"
#include "protocol/PeerSlot.h"

#include <cstdint>

namespace Multiplayer
{
    struct PlayerInspection
    {
        PeerSlot playerID{ 0 };
        std::uint16_t level{ 0 };
        // The player's space and location, as in their latest context; empty when unknown.
        FormReference interiorCell;
        FormReference worldspace;
        FormReference location;
    };
}
