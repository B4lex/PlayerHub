#include "forms/ItemInstance.h"
#include "forms/FormLookup.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace Forms
{
    namespace
    {
        using Multiplayer::TradeItem;
        using Multiplayer::TradeSkip;
        using Multiplayer::TradeSkipReason;

        struct TemperTier
        {
            float health;
            // Game settings that name the tier in the game's language.
            const char* weaponSetting;
            const char* armorSetting;
            std::string_view fallback;
        };

        // The tempering tiers the inventory names, by item health.
        constexpr std::array<TemperTier, 6> kTemperTiers{ {
            { 1.6F, "sHealthDataPrefixWeap6", "sHealthDataPrefixArmo6", "Legendary" },
            { 1.5F, "sHealthDataPrefixWeap5", "sHealthDataPrefixArmo5", "Epic" },
            { 1.4F, "sHealthDataPrefixWeap4", "sHealthDataPrefixArmo4", "Flawless" },
            { 1.3F, "sHealthDataPrefixWeap3", "sHealthDataPrefixArmo3", "Exquisite" },
            { 1.2F, "sHealthDataPrefixWeap2", "sHealthDataPrefixArmo2", "Superior" },
            { 1.1F, "sHealthDataPrefixWeap1", "sHealthDataPrefixArmo1", "Fine" },
        } };

        std::string_view GetTemperName(const TemperTier& a_tier, bool a_weapon)
        {
            auto* settings = RE::GameSettingCollection::GetSingleton();
            const auto* setting = settings ? settings->GetSetting(a_weapon ? a_tier.weaponSetting : a_tier.armorSetting)
                                           : nullptr;
            const char* name =
                setting && setting->GetType() == RE::Setting::Type::kString ? setting->GetString() : nullptr;
            return name && *name ? std::string_view{ name } : a_tier.fallback;
        }

        TradeSkip MissingFrom(TradeSkipReason a_reason, const Multiplayer::FormReference& a_reference)
        {
            return TradeSkip{ .reason = a_reason,
                .plugin = FormLookup::IsPluginLoaded(a_reference.plugin) ? std::string{} : a_reference.plugin };
        }

        std::string ReadCustomName(const RE::ExtraTextDisplayData& a_text)
        {
            if (!a_text.IsPlayerSet() || a_text.ownerQuest || a_text.displayNameText)
            {
                return {};
            }
            std::string_view name{ a_text.displayName.c_str() };
            // The display name continues with the tempering tier after the given name.
            name = name.substr(0, (std::min)(name.size(), static_cast<std::size_t>(a_text.customNameLength)));
            std::string result{ name.substr(0, (std::min)(name.size(), TradeItem::kMaximumNameBytes)) };
            std::erase_if(result, [](char a_character) {
                const auto byte = static_cast<unsigned char>(a_character);
                return byte < 0x20 || byte == 0x7F;
            });
            return result;
        }

        bool ReadEffects(const RE::MagicItem& a_magic, TradeItem& a_item)
        {
            if (a_magic.effects.size() > TradeItem::kMaximumEffects)
            {
                return false;
            }
            for (const auto* effect : a_magic.effects)
            {
                auto reference = effect ? FormLookup::MakeFormReference(effect->baseEffect) : std::nullopt;
                if (!reference)
                {
                    return false;
                }
                a_item.effects.push_back(Multiplayer::TradeEffect{ .effect = std::move(*reference),
                    .magnitude = effect->effectItem.magnitude,
                    .area = effect->effectItem.area,
                    .duration = effect->effectItem.duration,
                    .cost = effect->cost });
            }
            return !a_item.effects.empty();
        }

        bool ReadEnchantment(const RE::ExtraEnchantment& a_extra, TradeItem& a_item)
        {
            auto* enchantment = a_extra.enchantment;
            if (!enchantment)
            {
                return true;
            }
            a_item.maximumCharge = a_extra.charge;
            a_item.charge = a_extra.charge;
            if (!enchantment->IsDynamicForm())
            {
                auto reference = FormLookup::MakeFormReference(enchantment);
                if (!reference)
                {
                    return false;
                }
                a_item.enchantment = std::move(*reference);
                return true;
            }
            return ReadEffects(*enchantment, a_item);
        }

        std::expected<ResolvedItem, TradeSkip> ResolveEffects(const TradeItem& a_item, ResolvedItem a_resolved)
        {
            for (const auto& effect : a_item.effects)
            {
                auto* effectForm = FormLookup::ResolveForm(effect.effect);
                auto* setting = effectForm ? effectForm->As<RE::EffectSetting>() : nullptr;
                if (!setting)
                {
                    return std::unexpected(effectForm ? TradeSkip{ .reason = TradeSkipReason::kBadEnchantment }
                                                      : MissingFrom(TradeSkipReason::kMissingEffect, effect.effect));
                }
                a_resolved.effects.push_back(ResolvedEffect{ .effect = setting, .values = effect });
            }
            return a_resolved;
        }

        RE::BSTArray<RE::Effect> BuildEffects(const ResolvedItem& a_item)
        {
            RE::BSTArray<RE::Effect> effects;
            effects.reserve(static_cast<std::uint32_t>(a_item.effects.size()));
            for (const auto& resolved : a_item.effects)
            {
                auto& effect = effects.emplace_back();
                effect.baseEffect = resolved.effect;
                effect.effectItem.magnitude = resolved.values.magnitude;
                effect.effectItem.area = resolved.values.area;
                effect.effectItem.duration = resolved.values.duration;
                effect.cost = resolved.values.cost;
            }
            return effects;
        }

        // CommonLib's AddPoison takes the wrong smart pointer; the engine takes AddPotion's.
        void AddPoison(RE::BGSCreatedObjectManager& a_manager, RE::CreatedObjPtr<RE::AlchemyItem>& a_created,
            RE::BSTArray<RE::Effect>& a_effects)
        {
            using func_t = void(RE::BGSCreatedObjectManager*, RE::CreatedObjPtr<RE::AlchemyItem>&,
                RE::BSTArray<RE::Effect>&);
            static REL::Relocation<func_t> func{ RELOCATION_ID(35266, 36168) };
            func(&a_manager, a_created, a_effects);
        }

        const ResolvedEffect* GetCostliestEffect(const ResolvedItem& a_item)
        {
            const auto costliest = std::ranges::max_element(
                a_item.effects, {}, [](const ResolvedEffect& a_effect) { return a_effect.values.cost; });
            return costliest != a_item.effects.end() ? &*costliest : nullptr;
        }
    }

    std::optional<TradeCategory> GetTradeCategory(const RE::TESBoundObject& a_object)
    {
        switch (a_object.GetFormType())
        {
            case RE::FormType::Weapon:
            {
                const auto& weapon = static_cast<const RE::TESObjectWEAP&>(a_object);
                return weapon.GetPlayable() && !weapon.IsBound() ? std::optional{ TradeCategory::kWeapon }
                                                                 : std::nullopt;
            }
            case RE::FormType::Armor:
                return (a_object.formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0
                    ? std::optional{ TradeCategory::kArmor }
                    : std::nullopt;
            case RE::FormType::Ammo:
                return a_object.GetPlayable() ? std::optional{ TradeCategory::kAmmo } : std::nullopt;
            case RE::FormType::AlchemyItem:
                return TradeCategory::kPotion;
            case RE::FormType::Ingredient:
                return TradeCategory::kIngredient;
            case RE::FormType::Book:
                return TradeCategory::kBook;
            case RE::FormType::Scroll:
                return TradeCategory::kScroll;
            case RE::FormType::SoulGem:
                return TradeCategory::kSoulGem;
            case RE::FormType::KeyMaster:
                return TradeCategory::kKey;
            case RE::FormType::Misc:
                return TradeCategory::kMisc;
            case RE::FormType::Light:
                return static_cast<const RE::TESObjectLIGH&>(a_object).CanBeCarried() ? std::optional{ TradeCategory::kMisc }
                                                                                     : std::nullopt;
            default:
                return std::nullopt;
        }
    }

    std::expected<TradeItem, OfferBlock> DescribeItem(RE::TESBoundObject& a_object, const RE::ExtraDataList* a_extra)
    {
        if (a_object.IsDynamicForm())
        {
            auto* potion = a_object.As<RE::AlchemyItem>();
            TradeItem item{ .poison = potion && potion->IsPoison() };
            if (potion && ReadEffects(*potion, item) && Multiplayer::IsValidTradeItem(item))
            {
                return item;
            }
            return std::unexpected(potion ? OfferBlock::kUnreferenceable : OfferBlock::kPlayerMade);
        }
        auto reference = FormLookup::MakeFormReference(&a_object);
        if (!reference)
        {
            return std::unexpected(OfferBlock::kUnreferenceable);
        }
        TradeItem item{ .item = std::move(*reference) };
        if (!a_extra)
        {
            return item;
        }
        if (a_extra->HasType(RE::ExtraDataType::kAliasInstanceArray))
        {
            return std::unexpected(OfferBlock::kQuestItem);
        }
        if (const auto* enchantment = a_extra->GetByType<RE::ExtraEnchantment>();
            enchantment && !ReadEnchantment(*enchantment, item))
        {
            return std::unexpected(OfferBlock::kUnreferenceable);
        }
        if (const auto* charge = a_extra->GetByType<RE::ExtraCharge>(); charge && item.IsEnchanted())
        {
            item.charge = std::clamp(charge->charge, 0.0F, static_cast<float>(item.maximumCharge));
        }
        if (const auto* health = a_extra->GetByType<RE::ExtraHealth>())
        {
            item.health = std::clamp(health->health, 0.0F, TradeItem::kMaximumHealth);
        }
        if (const auto* text = a_extra->GetByType<RE::ExtraTextDisplayData>())
        {
            item.name = ReadCustomName(*text);
        }
        if (const auto* soul = a_extra->GetByType<RE::ExtraSoul>())
        {
            item.soul = static_cast<std::uint8_t>(
                (std::min)(static_cast<std::uint32_t>(soul->GetContainedSoul()), std::uint32_t{ TradeItem::kMaximumSoul }));
        }
        return item;
    }

    std::expected<ResolvedItem, TradeSkip> ResolveItem(const TradeItem& a_item)
    {
        if (a_item.IsPlayerMade())
        {
            return ResolveEffects(a_item, ResolvedItem{ .category = TradeCategory::kPotion });
        }
        auto* form = FormLookup::ResolveForm(a_item.item);
        if (!form)
        {
            return std::unexpected(MissingFrom(TradeSkipReason::kMissingItem, a_item.item));
        }
        auto* object = form->As<RE::TESBoundObject>();
        const auto category = object ? GetTradeCategory(*object) : std::nullopt;
        if (!category)
        {
            return std::unexpected(TradeSkip{ .reason = TradeSkipReason::kWrongType });
        }
        ResolvedItem resolved{ .object = object, .category = *category };
        if (!a_item.IsEnchanted())
        {
            return resolved;
        }
        if (*category != TradeCategory::kWeapon && *category != TradeCategory::kArmor)
        {
            return std::unexpected(TradeSkip{ .reason = TradeSkipReason::kBadEnchantment });
        }
        if (!a_item.enchantment.IsEmpty())
        {
            auto* enchantment = FormLookup::ResolveForm(a_item.enchantment);
            if (!enchantment)
            {
                return std::unexpected(MissingFrom(TradeSkipReason::kMissingEffect, a_item.enchantment));
            }
            resolved.enchantment = enchantment->As<RE::EnchantmentItem>();
            if (!resolved.enchantment)
            {
                return std::unexpected(TradeSkip{ .reason = TradeSkipReason::kBadEnchantment });
            }
            return resolved;
        }
        return ResolveEffects(a_item, std::move(resolved));
    }

    RE::EnchantmentItem* CreateEnchantment(const ResolvedItem& a_item)
    {
        auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
        if (!manager || a_item.effects.empty())
        {
            return nullptr;
        }
        auto effects = BuildEffects(a_item);
        // The manager returns the enchantment it already holds for the same effects, as
        // enchanting at a table does.
        return a_item.category == TradeCategory::kWeapon ? manager->AddWeaponEnchantment(effects)
                                                         : manager->AddArmorEnchantment(effects);
    }

    RE::CreatedObjPtr<RE::AlchemyItem> CreateAlchemyItem(const ResolvedItem& a_resolved, const TradeItem& a_item)
    {
        RE::CreatedObjPtr<RE::AlchemyItem> created;
        auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
        if (!manager || a_resolved.effects.empty())
        {
            return created;
        }
        auto effects = BuildEffects(a_resolved);
        if (a_item.poison)
        {
            AddPoison(*manager, created, effects);
        }
        else
        {
            manager->AddPotion(created, effects);
        }
        return created;
    }

    void ApplyExtraData(RE::TESObjectREFR& a_reference, const ResolvedItem& a_resolved, const TradeItem& a_item,
        RE::EnchantmentItem* a_enchantment)
    {
        auto& extra = a_reference.extraList;
        if (a_enchantment)
        {
            extra.SetEnchantment(a_enchantment, a_item.maximumCharge, false);
            if (a_resolved.category == TradeCategory::kWeapon && a_item.charge < a_item.maximumCharge)
            {
                auto* charge = new RE::ExtraCharge();
                charge->charge = a_item.charge;
                extra.Add(charge);
            }
        }
        if (a_item.health > 0.0F)
        {
            extra.Add(new RE::ExtraHealth(a_item.health));
        }
        if (a_item.soul != 0)
        {
            extra.Add(new RE::ExtraSoul(static_cast<RE::SOUL_LEVEL>(a_item.soul)));
        }
        if (!a_item.name.empty())
        {
            extra.SetOverrideName(a_item.name.c_str());
        }
    }

    std::string GetDisplayName(const ResolvedItem& a_resolved, const TradeItem& a_item)
    {
        if (!a_resolved.object)
        {
            const auto* costliest = GetCostliestEffect(a_resolved);
            return costliest ? std::string{ costliest->effect->GetName() } : std::string{};
        }
        auto& object = *a_resolved.object;
        std::string name = a_item.name.empty() ? std::string{ object.GetName() } : a_item.name;
        const auto tier = std::ranges::find_if(
            kTemperTiers, [&](const TemperTier& a_tier) { return a_item.health >= a_tier.health - 0.001F; });
        if (tier != kTemperTiers.end())
        {
            name += std::format(" ({})", GetTemperName(*tier, object.IsWeapon()));
        }
        return name;
    }
}
