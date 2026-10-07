#pragma once

#include "forms/ItemInstance.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Forms
{
    struct ItemEffect
    {
        std::string text;
        // False for an ingredient effect the local player has not discovered.
        bool known{ true };
    };

    // An item's own numbers, without the viewing player's skills or perks.
    struct ItemDetails
    {
        enum class Rating : std::uint8_t
        {
            kNone,
            kDamage,
            kArmor
        };

        Rating rating{ Rating::kNone };
        std::int32_t ratingValue{ 0 };
        float weight{ 0.0F };
        std::int32_t value{ 0 };
        std::uint32_t charge{ 0 };
        std::uint32_t maximumCharge{ 0 };
        std::vector<ItemEffect> effects;
    };

    // Game thread. a_item supplies the instance data: tempering, charge, and a player
    // enchantment's effects in a_resolved.
    [[nodiscard]] ItemDetails GetItemDetails(const ResolvedItem& a_resolved, const Multiplayer::TradeItem& a_item);
}
