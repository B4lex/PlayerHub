#include "forms/ItemDetails.h"
#include "text/AsciiCase.h"
#include "text/Text.h"

#include <algorithm>
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

        // Skyrim.esm ArmorCuirass, the kKeywordCuirass default object.
        constexpr RE::FormID kCuirassKeywordID = 0x0006C0EC;
        // Initial values of the static game settings below, which the collection always holds.
        constexpr float kDefaultSmithingMax = 10.0F;
        constexpr float kDefaultEnchantmentPointsMult = 0.4F;

        // The formulas below are ported from the engine functions named on each, which compute the
        // same on 1.7.104 and SE 1.5.97.

        // Health from fHealthDataValue1 (0) to fHealthDataValue6 (1). Without both settings the
        // engine measures from 1 over 1, and over 1 when they are equal.
        float GetTemperProgress(float a_health)
        {
            const auto lowest = "fHealthDataValue1"_gs;
            const auto highest = "fHealthDataValue6"_gs;
            if (!lowest || !highest)
            {
                return a_health - 1.0F;
            }
            const auto range = *highest - *lowest;
            return (a_health - *lowest) / (range != 0.0F ? range : 1.0F);
        }

        // ID 26498 (SE 25915): the damage tempering adds before skills and perks.
        float GetWeaponTemperBonus(float a_health)
        {
            const auto maximum = "fSmithingWeaponMax"_gs.value_or(kDefaultSmithingMax);
            return (std::max)(GetTemperProgress(a_health) * (maximum - 1.0F) + 1.0F, 0.0F);
        }

        // ID 26499 (SE 25916): the same bonus for armor, doubled for armor with the kKeywordCuirass
        // default object.
        float GetArmorTemperBonus(float a_health, const RE::TESObjectARMO& a_armor)
        {
            const auto maximum = "fSmithingArmorMax"_gs.value_or(kDefaultSmithingMax);
            auto bonus = GetTemperProgress(a_health) * (maximum - 1.0F) + 1.0F;
            if (a_armor.HasKeywordID(kCuirassKeywordID))
            {
                bonus += bonus;
            }
            return (std::max)(bonus, 0.0F);
        }

        // ID 26483 (SE 25901): the gold value scaled by tempering, unscaled without both health settings.
        // InventoryEntryData::GetValue (ID 15995) rounds it up.
        float GetTemperedValue(float a_value, float a_health)
        {
            const auto lowest = "fHealthDataValue1"_gs;
            const auto highest = "fHealthDataValue6"_gs;
            if (!lowest || !highest || !(*highest > 1.0F))
            {
                return a_value;
            }
            return ((a_health - 1.0F) / (*highest - 1.0F) + 1.0F) * a_value;
        }

        // ID 26465 (SE 25893): the value of an item with a cast enchantment, from its tempered value,
        // maximum charge, and enchantment cost. GetValue truncates it.
        float GetChargedValue(float a_value, float a_maximumCharge, float a_cost)
        {
            if (a_value == 0.0F)
            {
                return 0.0F;
            }
            return "fEnchantmentPointsMult"_gs.value_or(kDefaultEnchantmentPointsMult) * a_maximumCharge + a_value
                + "fEnchantmentEffectPointsMult"_gs.value_or(kDefaultEnchantmentPointsMult) * a_cost;
        }

        // ID 51905: the item card shows a magnitude truncated to an integer, after scaling it by 100
        // for an actor value flagged kDisplayedEffectMagnitudeTimesOneHundred.
        std::int64_t GetDisplayedMagnitude(const RE::EffectSetting& a_effect, float a_magnitude)
        {
            const auto* info = RE::ActorValueList::GetActorValueInfo(a_effect.data.primaryAV);
            if (info && info->flags.all(RE::ActorValueInfo::ActorValueFlag::kDisplayedEffectMagnitudeTimesOneHundred))
            {
                a_magnitude *= 100.0F;
            }
            return static_cast<std::int64_t>(a_magnitude);
        }

        template <class Replace>
        std::string ReplaceBracketed(std::string_view a_text, char a_open, char a_close, Replace a_replace)
        {
            std::string result;
            std::size_t position = 0;
            for (auto open = a_text.find(a_open); open != std::string_view::npos; open = a_text.find(a_open, position))
            {
                const auto close = a_text.find(a_close, open + 1);
                if (close == std::string_view::npos)
                {
                    break;
                }
                result.append(a_text.substr(position, open - position));
                result.append(a_replace(a_text.substr(open + 1, close - open - 1)));
                position = close + 1;
            }
            result.append(a_text.substr(position));
            return result;
        }

        void AddEffect(std::vector<ItemEffect>& a_effects, const RE::EffectSetting& a_effect, float a_magnitude,
            std::uint32_t a_area, std::uint32_t a_duration)
        {
            if (a_effect.data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
            {
                return;
            }
            const auto fillTag = [&](std::string_view a_tag) {
                if (AsciiCase::Equals(a_tag, "mag"))
                {
                    return std::format("{}", GetDisplayedMagnitude(a_effect, a_magnitude));
                }
                if (AsciiCase::Equals(a_tag, "dur"))
                {
                    return std::format("{}", a_duration);
                }
                if (AsciiCase::Equals(a_tag, "area"))
                {
                    return std::format("{}", a_area);
                }
                return std::string{ a_tag };
            };
            const auto filled = ReplaceBracketed(a_effect.magicItemDescription.c_str(), '<', '>', fillTag);
            auto text = ReplaceBracketed(filled, '[', ']', [](std::string_view) { return std::string{}; });
            if (text.find_first_not_of(" \n") != std::string::npos)
            {
                a_effects.push_back(ItemEffect{ .text = std::move(text) });
            }
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
                    a_effects.push_back(
                        ItemEffect{ .text = std::string{ Text::Get(Text::Id::kItemUnknownEffect) }, .known = false });
                    continue;
                }
                AddEffect(a_effects, *effect->baseEffect, effect->effectItem.magnitude, effect->effectItem.area,
                    effect->effectItem.duration);
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
        // TESForm::GetWeight (14809/14988) returns -1 for a weightless form: ammo, and one Misc item it
        // compares against a cached form, unless Survival Mode is on.
        details.weight = (std::max)(object->GetWeight(), 0.0F);

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
                    AddEffect(details.effects, *effect.effect, effect.values.magnitude, effect.values.area,
                        effect.values.duration);
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
