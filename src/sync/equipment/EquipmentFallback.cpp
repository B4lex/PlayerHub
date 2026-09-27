#include "sync/equipment/EquipmentFallback.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "forms/FormLookup.h"
#include "forms/ItemForms.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace EquipmentFallback
{
    namespace
    {
        using Settings = Config::EquipmentFallbackSettings;
        using Multiplayer::FormReference;

        // Slot indices from slot 30. When the fallbacks of one missing item
        // overlap, the earlier slot wins, so the most visible pieces go first.
        constexpr auto kArmorSlotOrder = [] {
            constexpr std::array<std::uint32_t, 6> kMainSlots{ 2, 0, 1, 3, 7, 9 }; // Body Head Hair Hands Feet Shield
            std::array<std::uint32_t, Settings::kArmorSlotCount> order{};
            std::size_t next = 0;
            for (const auto slot : kMainSlots)
            {
                order[next++] = slot;
            }
            for (std::uint32_t slot = 0; slot < Settings::kArmorSlotCount; ++slot)
            {
                if (std::ranges::find(kMainSlots, slot) == kMainSlots.end())
                {
                    order[next++] = slot;
                }
            }
            return order;
        }();

        struct ResolvedFallbacks
        {
            // Indexed by slot from 30, then by Multiplayer::EquipmentArmorType, with
            // the slot's any-type item already applied to empty types.
            std::array<std::array<RE::TESObjectARMO*, Settings::kArmorTypeCount>, Settings::kArmorSlotCount> armor{};
            std::array<RE::TESObjectWEAP*, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons{};
            RE::TESObjectLIGH* torch{ nullptr };
            RE::TESAmmo* arrow{ nullptr };
            RE::TESAmmo* bolt{ nullptr };
        };

        ResolvedFallbacks resolved;
        // The configuration the items were resolved from; null before Initialize.
        std::shared_ptr<const Config::PluginConfig> resolvedConfig;
        std::uint64_t resolvedConfigRevision{ 0 };
        std::uint64_t generation{ 0 };

        // Resolves one configured item, or logs why it cannot be used. The
        // settings window offers only items a_filter matches.
        template <class T>
        T* ResolveConfigured(const FormReference& a_reference, std::string_view a_label,
            const Forms::ItemFilter& a_filter, std::uint32_t& a_unusable)
        {
            if (a_reference.IsEmpty())
            {
                return nullptr;
            }
            auto* form = FormLookup::ResolveForm(a_reference);
            const auto traits = form ? Forms::GetItemTraits(*form) : std::nullopt;
            T* accepted = traits && a_filter.Matches(*traits) ? form->As<T>() : nullptr;
            if (!accepted)
            {
                ++a_unusable;
                logs::warn("Equipment fallback ignored [EquipmentFallback] {}: item={}|{:06X} reason={}", a_label,
                    a_reference.plugin, a_reference.localFormID, form ? "does-not-fit" : "missing");
            }
            return accepted;
        }
    }

    void Initialize()
    {
        resolvedConfigRevision = Config::GetRevision();
        resolvedConfig = Config::Get();
        ++generation;
        const auto& settings = resolvedConfig->equipmentFallback;
        resolved = {};
        if (!settings.enabled)
        {
            logs::info("Equipment fallback is disabled; unresolvable remote items are left off");
            return;
        }

        std::uint32_t unusable = 0;
        for (std::size_t slot = 0; slot < settings.armor.size(); ++slot)
        {
            const auto& configured = settings.armor[slot];
            auto* anyType = ResolveConfigured<RE::TESObjectARMO>(configured.anyArmorType,
                Config::GetArmorFallbackKey(slot, std::nullopt), Config::GetArmorFallbackFilter(slot, std::nullopt),
                unusable);
            for (std::size_t type = 0; type < configured.byArmorType.size(); ++type)
            {
                const auto armorType = static_cast<Multiplayer::EquipmentArmorType>(type);
                auto* armor = ResolveConfigured<RE::TESObjectARMO>(configured.byArmorType[type],
                    Config::GetArmorFallbackKey(slot, armorType), Config::GetArmorFallbackFilter(slot, armorType),
                    unusable);
                resolved.armor[slot][type] = armor ? armor : anyType;
            }
        }

        for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < settings.weapons.size();
            ++type)
        {
            resolved.weapons[type] = ResolveConfigured<RE::TESObjectWEAP>(settings.weapons[type],
                Config::GetWeaponFallbackKey(type), Config::GetWeaponFallbackFilter(type), unusable);
        }
        resolved.torch = ResolveConfigured<RE::TESObjectLIGH>(settings.torch, "Torch", Forms::kTorchFilter, unusable);
        resolved.arrow = ResolveConfigured<RE::TESAmmo>(settings.arrow, "Arrow", Forms::kArrowFilter, unusable);
        resolved.bolt = ResolveConfigured<RE::TESAmmo>(settings.bolt, "Bolt", Forms::kBoltFilter, unusable);

        logs::info("Equipment fallback initialized: unusableItems={} generation={}", unusable,
            static_cast<unsigned long long>(generation));
    }

    void Refresh()
    {
        // Apply publishes the configuration before its revision, so a snapshot
        // taken after reading the revision is at least that new.
        const auto revision = Config::GetRevision();
        if (!resolvedConfig || revision == resolvedConfigRevision)
        {
            return;
        }
        if (Config::Get()->equipmentFallback == resolvedConfig->equipmentFallback)
        {
            resolvedConfigRevision = revision;
            return;
        }
        logs::info("Equipment fallback settings changed; resolving the configured items again");
        Initialize();
    }

    std::uint64_t GetGeneration()
    {
        return generation;
    }

    void AppendArmor(const Multiplayer::EquipmentArmor& a_entry, std::uint32_t& a_occupiedSlots,
        std::vector<RE::TESObjectARMO*>& a_armor)
    {
        const auto type = static_cast<std::size_t>(a_entry.armorType);
        if (type >= Settings::kArmorTypeCount)
        {
            return;
        }
        for (const auto slot : kArmorSlotOrder)
        {
            const auto slotBit = 1U << slot;
            if ((a_entry.slotMask & slotBit) == 0 || (a_occupiedSlots & slotBit) != 0)
            {
                continue;
            }
            auto* armor = resolved.armor[slot][type];
            if (!armor)
            {
                continue;
            }
            // Equipping over an occupied slot would unequip the item shown there.
            const auto armorSlots = armor->GetSlotMask().underlying();
            if ((armorSlots & a_occupiedSlots) != 0)
            {
                continue;
            }
            a_occupiedSlots |= armorSlots;
            a_armor.push_back(armor);
        }
    }

    RE::TESBoundObject* FindHand(const Multiplayer::EquipmentHand& a_entry)
    {
        switch (a_entry.kind)
        {
            case Multiplayer::EquipmentHandKind::kWeapon:
                return a_entry.weaponType < resolved.weapons.size() ? resolved.weapons[a_entry.weaponType] : nullptr;
            case Multiplayer::EquipmentHandKind::kTorch:
                return resolved.torch;
            default:
                return nullptr;
        }
    }

    RE::TESAmmo* FindAmmo(bool a_bolt)
    {
        return a_bolt ? resolved.bolt : resolved.arrow;
    }
}
