#pragma once

#include "protocol/TradeState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace Forms
{
    enum class TradeCategory : std::uint8_t
    {
        kWeapon,
        kArmor,
        kAmmo,
        kPotion,
        kIngredient,
        kBook,
        kScroll,
        kSoulGem,
        kKey,
        kMisc
    };

    // Why an inventory item cannot be offered.
    enum class OfferBlock : std::uint8_t
    {
        kQuestItem,
        kPlayerMade,
        kUnreferenceable
    };

    [[nodiscard]] std::optional<TradeCategory> GetTradeCategory(const RE::TESBoundObject& a_object);

    // The item an inventory stack holds, with a count of one.
    [[nodiscard]] std::expected<Multiplayer::TradeItem, OfferBlock> DescribeItem(
        RE::TESBoundObject& a_object, const RE::ExtraDataList* a_extra);

    struct ResolvedEffect
    {
        RE::EffectSetting* effect{ nullptr };
        Multiplayer::TradeEffect values;
    };

    struct ResolvedItem
    {
        RE::TESBoundObject* object{ nullptr };
        RE::EnchantmentItem* enchantment{ nullptr };
        // A player enchantment, built only when the item is received.
        std::vector<ResolvedEffect> effects;
        TradeCategory category{ TradeCategory::kMisc };
    };

    // Looks the item up in the local game without creating anything. The skip's index is
    // left for the caller.
    [[nodiscard]] std::expected<ResolvedItem, Multiplayer::TradeSkip> ResolveItem(const Multiplayer::TradeItem& a_item);

    [[nodiscard]] RE::EnchantmentItem* CreateEnchantment(const ResolvedItem& a_item);

    // Puts the item's extra data on a reference, which keeps it when picked up.
    void ApplyExtraData(RE::TESObjectREFR& a_reference, const ResolvedItem& a_resolved,
        const Multiplayer::TradeItem& a_item, RE::EnchantmentItem* a_enchantment);

    [[nodiscard]] std::string GetDisplayName(RE::TESBoundObject& a_object, const Multiplayer::TradeItem& a_item);
}
