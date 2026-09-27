#include "forms/ItemForms.h"
#include "forms/FormLookup.h"
#include "forms/ItemCatalog.h"
#include "runtime/Profiling.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Forms
{
    namespace
    {
        template <class T>
        void AppendNamedItems(RE::TESDataHandler& a_dataHandler, std::vector<CatalogItem>& a_items)
        {
            for (auto* form : a_dataHandler.GetFormArray<T>())
            {
                if (!form)
                {
                    continue;
                }
                // Enchanted variants look like the template item, which is listed.
                if constexpr (std::is_same_v<T, RE::TESObjectARMO>)
                {
                    if (form->templateArmor)
                    {
                        continue;
                    }
                }
                else if constexpr (std::is_same_v<T, RE::TESObjectWEAP>)
                {
                    if (form->templateWeapon)
                    {
                        continue;
                    }
                }
                const char* name = form->GetName();
                if (!name || name[0] == '\0')
                {
                    continue;
                }
                auto traits = GetItemTraits(*form);
                auto reference = FormLookup::MakeFormReference(form);
                if (traits && reference)
                {
                    a_items.push_back(CatalogItem{ .reference = std::move(*reference),
                        .formID = form->GetFormID(),
                        .name = name,
                        .traits = *traits });
                }
            }
        }
    }

    std::optional<ItemTraits> GetItemTraits(RE::TESForm& a_form)
    {
        if (const auto* armor = a_form.As<RE::TESObjectARMO>())
        {
            const auto slotMask = static_cast<std::uint32_t>(armor->GetSlotMask().underlying());
            if (slotMask == 0)
            {
                return std::nullopt;
            }
            return ItemTraits{ .kind = ItemKind::kArmor, .slotMask = slotMask, .armorType = GetArmorType(*armor) };
        }
        if (const auto* weapon = a_form.As<RE::TESObjectWEAP>())
        {
            const auto type = static_cast<std::uint8_t>(weapon->GetWeaponType());
            if (type < Multiplayer::PlayerEquipment::kMinimumWeaponType
                || type > Multiplayer::PlayerEquipment::kMaximumWeaponType)
            {
                return std::nullopt;
            }
            return ItemTraits{ .kind = ItemKind::kWeapon, .weaponType = type };
        }
        if (const auto* light = a_form.As<RE::TESObjectLIGH>())
        {
            return light->CanBeCarried() ? std::optional{ ItemTraits{ .kind = ItemKind::kTorch } } : std::nullopt;
        }
        if (auto* ammo = a_form.As<RE::TESAmmo>())
        {
            return ItemTraits{ .kind = ItemKind::kAmmo, .bolt = ammo->IsBolt() };
        }
        return std::nullopt;
    }

    Multiplayer::EquipmentArmorType GetArmorType(const RE::TESObjectARMO& a_armor)
    {
        switch (a_armor.GetArmorType())
        {
            case RE::BIPED_MODEL::ArmorType::kLightArmor:
                return Multiplayer::EquipmentArmorType::kLight;
            case RE::BIPED_MODEL::ArmorType::kHeavyArmor:
                return Multiplayer::EquipmentArmorType::kHeavy;
            default:
                return Multiplayer::EquipmentArmorType::kClothing;
        }
    }

    void BuildItemCatalog()
    {
        PROFILE_SCOPE("BuildItemCatalog");
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logs::warn("Item catalog not built: no data handler");
            return;
        }

        std::vector<CatalogItem> items;
        AppendNamedItems<RE::TESObjectARMO>(*dataHandler, items);
        AppendNamedItems<RE::TESObjectWEAP>(*dataHandler, items);
        AppendNamedItems<RE::TESObjectLIGH>(*dataHandler, items);
        AppendNamedItems<RE::TESAmmo>(*dataHandler, items);

        std::vector<std::string_view> plugins;
        const auto appendPlugins = [&plugins](const RE::TESFile* const* a_files, std::size_t a_count) {
            for (std::size_t index = 0; index < a_count; ++index)
            {
                if (a_files[index])
                {
                    plugins.push_back(a_files[index]->GetFilename());
                }
            }
        };
        appendPlugins(std::as_const(*dataHandler).GetLoadedMods(), dataHandler->GetLoadedModCount());
        appendPlugins(std::as_const(*dataHandler).GetLoadedLightMods(), dataHandler->GetLoadedLightModCount());

        const auto itemCount = items.size();
        PublishItemCatalog(std::make_shared<const ItemCatalog>(std::move(items), plugins));
        logs::info("Item catalog built: items={} plugins={}", itemCount, plugins.size());
    }
}
