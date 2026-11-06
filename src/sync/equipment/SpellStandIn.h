#pragma once

#include "protocol/EquipmentState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string_view>

namespace SpellStandIn
{
    [[nodiscard]] Multiplayer::EquipmentHand Capture(const RE::MagicItem& a_spell, std::uint32_t& a_skipped);
    // Adds a staff's enchantment and its look to the staff's weapon hand.
    void CaptureStaff(const RE::MagicItem& a_enchantment, Multiplayer::EquipmentHand& a_hand, std::uint32_t& a_skipped);

    struct Resolution
    {
        // A spell hand's SpellItem, or a staff's enchantment or SpellItem stand-in.
        RE::MagicItem* spell{ nullptr };
        // "native", or for a stand-in "spell", "look", or "placeholder".
        std::string_view source;
        // Why the source's spell was not used.
        std::string_view dropReason;
    };

    // a_allowNative: the hand may hold the receiver's own version of the source's
    // spell, which casts natively, instead of a stand-in.
    [[nodiscard]] Resolution Resolve(const Multiplayer::EquipmentHand& a_hand, bool a_allowNative);
    // What a staff hand casts, from its enchantment and look.
    [[nodiscard]] Resolution ResolveStaff(const Multiplayer::EquipmentHand& a_hand);
}
