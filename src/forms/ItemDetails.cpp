#include "forms/ItemDetails.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string_view>

namespace Forms
{
    namespace
    {
        using namespace RE::literals;

        // ExtraHealth is 1 on an untempered item; a TradeItem leaves it at 0.
        constexpr float kUntemperedHealth = 1.0F;

        // 1.7.104 ID 26498: the damage tempering adds before skills and perks,
        // max(0, (health - fHealthDataValue1) / (fHealthDataValue6 - fHealthDataValue1) * 9 + 1).
        float GetWeaponTemperBonus(float a_health)
        {
            using Function = float(float);
            static REL::Relocation<Function*> function{ REL::ID(26498) };
            return function(a_health);
        }

        // 1.7.104 ID 26499: the same bonus for armor, doubled for armor with the
        // kKeywordCuirass default object (index 318 in this runtime's table).
        float GetArmorTemperBonus(float a_health, RE::TESObjectARMO& a_armor)
        {
            using Function = float(float, RE::TESObjectARMO*);
            static REL::Relocation<Function*> function{ REL::ID(26499) };
            return function(a_health, std::addressof(a_armor));
        }

        // 1.7.104 ID 26483: the gold value scaled by tempering. InventoryEntryData::GetValue
        // (ID 15995) rounds it up.
        float GetTemperedValue(float a_value, float a_health)
        {
            using Function = float(float, float);
            static REL::Relocation<Function*> function{ REL::ID(26483) };
            return function(a_value, a_health);
        }

        // 1.7.104 ID 26465: the value of an item with a cast enchantment, from its tempered value,
        // maximum charge, and enchantment cost. GetValue truncates it.
        float GetChargedValue(float a_value, float a_maximumCharge, float a_cost)
        {
            using Function = float(float, float, float);
            static REL::Relocation<Function*> function{ REL::ID(26465) };
            return function(a_value, a_maximumCharge, a_cost);
        }

        std::string FormatNumber(float a_value)
        {
            const auto rounded = std::round(a_value);
            return std::abs(a_value - rounded) < 0.05F ? std::format("{}", static_cast<std::int64_t>(rounded))
                                                       : std::format("{:.1f}", a_value);
        }

        void ReplaceTag(std::string& a_text, std::string_view a_tag, const std::string& a_value)
        {
            const auto equal = [](char a_left, char a_right) {
                return std::tolower(static_cast<unsigned char>(a_left))
                    == std::tolower(static_cast<unsigned char>(a_right));
            };
            for (auto found = std::ranges::search(a_text, a_tag, equal); !found.empty();
                found = std::ranges::search(a_text, a_tag, equal))
            {
                const auto position = static_cast<std::size_t>(found.begin() - a_text.begin());
                a_text.replace(position, a_tag.size(), a_value);
            }
        }

        // The effect's item description (DNAM) with its <mag>, <dur>, and <area> tags filled,
        // as the game's item card shows it, or its name when it has none.
        std::string DescribeEffect(
            const RE::EffectSetting& a_effect, float a_magnitude, std::uint32_t a_area, std::uint32_t a_duration)
        {
            std::string text{ a_effect.magicItemDescription.c_str() };
            if (text.empty())
            {
                return std::string{ a_effect.GetFullName() };
            }
            ReplaceTag(text, "<mag>", FormatNumber(a_magnitude));
            ReplaceTag(text, "<dur>", std::format("{}", a_duration));
            ReplaceTag(text, "<area>", std::format("{}", a_area));
            return text;
        }

        void AddEffects(const RE::MagicItem& a_magic, std::vector<ItemEffect>& a_effects)
        {
            const auto* ingredient = a_magic.As<RE::IngredientItem>();
            for (std::size_t i = 0; i < a_magic.effects.size(); ++i)
            {
                const auto* effect = a_magic.effects[static_cast<std::uint32_t>(i)];
                if (!effect || !effect->baseEffect)
                {
                    continue;
                }
                if (ingredient && (ingredient->gamedata.knownEffectFlags & (1U << i)) == 0)
                {
                    a_effects.push_back(ItemEffect{ .text = "Unknown effect", .known = false });
                    continue;
                }
                a_effects.push_back(
                    ItemEffect{ .text = DescribeEffect(*effect->baseEffect, effect->effectItem.magnitude,
                                    effect->effectItem.area, effect->effectItem.duration) });
            }
        }

        std::int32_t GetValue(RE::TESBoundObject& a_object, float a_health, const RE::MagicItem* a_enchantment,
            float a_effectsCost, bool a_enchanted, std::uint32_t a_maximumCharge)
        {
            const auto tempered = GetTemperedValue(static_cast<float>(a_object.GetGoldValue()), a_health);
            const auto value = tempered > 0.0F ? static_cast<std::int32_t>(std::ceil(tempered)) : 0;
            if (!a_enchanted || value <= 0)
            {
                return value;
            }
            const auto cost = a_enchantment ? a_enchantment->CalculateTotalGoldValue(nullptr) : a_effectsCost;
            const bool constant = a_enchantment
                ? a_enchantment->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect
                : !a_object.Is(RE::FormType::Weapon);
            if (constant)
            {
                return value + static_cast<std::int32_t>(std::floor(cost + 0.5F));
            }
            return static_cast<std::int32_t>(
                GetChargedValue(static_cast<float>(value), static_cast<float>(a_maximumCharge), cost));
        }
    }

    ItemDetails GetItemDetails(const ResolvedItem& a_resolved, const Multiplayer::TradeItem& a_item)
    {
        ItemDetails details;
        auto* object = a_resolved.object;
        if (!object)
        {
            return details;
        }
        const auto health = (std::max)(a_item.health, kUntemperedHealth);
        details.weight = object->GetWeight();

        const RE::EnchantmentItem* enchantment = a_resolved.enchantment;
        if (!enchantment && a_resolved.effects.empty())
        {
            if (const auto* enchantable = object->As<RE::TESEnchantableForm>())
            {
                enchantment = enchantable->formEnchanting;
            }
        }
        const bool enchanted = enchantment || !a_resolved.effects.empty();

        if (auto* weapon = object->As<RE::TESObjectWEAP>())
        {
            if (!weapon->IsStaff())
            {
                details.rating = ItemDetails::Rating::kDamage;
                details.ratingValue = static_cast<std::int32_t>(
                    std::round(static_cast<float>(weapon->GetAttackDamage()) * "fDamageWeaponMult"_gs.value_or(1.0F)
                        + GetWeaponTemperBonus(health)));
            }
            if (enchanted)
            {
                // A plugin weapon's own enchantment carries no ExtraCharge, and arrives full.
                details.maximumCharge = a_item.maximumCharge > 0 ? a_item.maximumCharge : weapon->amountofEnchantment;
                details.charge
                    = a_item.maximumCharge > 0 ? static_cast<std::uint32_t>(a_item.charge) : details.maximumCharge;
            }
        }
        else if (const auto* ammo = object->As<RE::TESAmmo>())
        {
            details.rating = ItemDetails::Rating::kDamage;
            details.ratingValue = static_cast<std::int32_t>(std::round(ammo->GetRuntimeData().data.damage));
        }
        else if (auto* armor = object->As<RE::TESObjectARMO>(); armor && armor->armorRating > 0)
        {
            details.rating = ItemDetails::Rating::kArmor;
            details.ratingValue = static_cast<std::int32_t>(
                std::ceil(static_cast<float>(armor->armorRating) / 100.0F + GetArmorTemperBonus(health, *armor)));
        }

        float effectsCost = 0.0F;
        if (enchantment)
        {
            AddEffects(*enchantment, details.effects);
        }
        else if (!a_resolved.effects.empty())
        {
            for (const auto& effect : a_resolved.effects)
            {
                if (effect.effect)
                {
                    details.effects.push_back(
                        ItemEffect{ .text = DescribeEffect(*effect.effect, effect.values.magnitude, effect.values.area,
                                        effect.values.duration) });
                }
                effectsCost += effect.values.cost;
            }
        }
        else if (const auto* magic = object->As<RE::MagicItem>())
        {
            AddEffects(*magic, details.effects);
        }

        details.value = GetValue(*object, health, enchantment, effectsCost, enchanted, details.maximumCharge);
        return details;
    }
}
