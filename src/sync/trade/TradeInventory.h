#pragma once

#include "forms/ItemInstance.h"
#include "protocol/TradeState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TradeInventory
{
    struct Row
    {
        Multiplayer::TradeItem item;
        RE::FormID formID{ 0 };
        std::string name;
        Forms::TradeCategory category{ Forms::TradeCategory::kMisc };
        float weight{ 0.0F };
        bool equipped{ false };
        bool favorited{ false };
        std::optional<Forms::OfferBlock> block;
    };

    // One row per distinct item, counting all of it the player holds.
    [[nodiscard]] std::vector<Row> Capture(RE::PlayerCharacter& a_player);

    [[nodiscard]] std::uint32_t Count(RE::PlayerCharacter& a_player, const Multiplayer::TradeItem& a_item);

    // Returns how many were removed, unequipped ones first.
    std::uint32_t Remove(RE::PlayerCharacter& a_player, const Multiplayer::TradeItem& a_item);

    std::uint32_t Add(
        RE::PlayerCharacter& a_player, const Forms::ResolvedItem& a_resolved, const Multiplayer::TradeItem& a_item);
}
