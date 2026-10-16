#pragma once

#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string_view>

namespace SpellStandIn
{
    [[nodiscard]] Multiplayer::EquipmentHand Capture(const RE::MagicItem& a_spell, std::uint32_t& a_skipped);

    struct Resolution
    {
        RE::SpellItem* spell{ nullptr };
        // "spell", "look", or "placeholder".
        std::string_view source;
        // Why the source's spell was not used.
        std::string_view dropReason;
    };

    [[nodiscard]] Resolution Resolve(const Multiplayer::EquipmentHand& a_hand);
}
