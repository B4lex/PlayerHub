#include "sync/equipment/DefaultOutfit.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "forms/FormLookup.h"
#include "forms/ItemForms.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace DefaultOutfit
{
    namespace
    {
        using Settings = Config::DefaultOutfitSettings;
        using Multiplayer::EquipmentHandKind;
        using Multiplayer::FormReference;

        // Indices into Settings::kArmorSlotMasks. Receivers apply pieces in this
        // order, and when the pieces for one missing item overlap, the earlier
        // slot wins, so the most visible pieces go first: Body, Head, Hands,
        // Feet, Shield, Amulet, Circlet, Backpack.
        constexpr std::array<std::size_t, Settings::kArmorSlotCount> kArmorSlotOrder{ 1, 0, 2, 4, 5, 3, 6, 7 };

        // The configured items that resolved and fit their key, else the key's
        // default if that does, else empty.
        Settings local;
        // The configuration the items were resolved from; null before Initialize.
        std::shared_ptr<const Config::PluginConfig> resolvedConfig;
        std::uint64_t resolvedConfigRevision{ 0 };

        // Why an item cannot be used for a key, or empty when it can.
        std::string_view FindUnusableReason(const FormReference& a_reference, const Forms::ItemFilter& a_filter)
        {
            auto* form = FormLookup::ResolveForm(a_reference);
            if (!form)
            {
                return "missing";
            }
            const auto traits = Forms::GetItemTraits(*form);
            return traits && a_filter.Matches(*traits) ? std::string_view{} : "does-not-fit";
        }

        FormReference ResolveConfigured(const FormReference& a_reference, const FormReference& a_default,
            std::string_view a_label, const Forms::ItemFilter& a_filter, std::uint32_t& a_unusable)
        {
            const auto reason = FindUnusableReason(a_reference, a_filter);
            if (reason.empty())
            {
                return a_reference;
            }
            ++a_unusable;
            // Anniversary Edition defaults are missing from Special Edition installs
            // without the upgrade.
            const bool useDefault = a_default != a_reference && FindUnusableReason(a_default, a_filter).empty();
            logs::warn("Default outfit item ignored [DefaultOutfit] {}: item={}|{:06X} reason={} using={}", a_label,
                a_reference.plugin, a_reference.localFormID, reason, useDefault ? "default" : "none");
            return useDefault ? a_default : FormReference{};
        }

        bool IsBaseGameItem(const FormReference& a_reference)
        {
            return Multiplayer::IsBaseGamePlugin(a_reference.plugin);
        }
    }

    void Initialize()
    {
        resolvedConfigRevision = Config::GetRevision();
        resolvedConfig = Config::Get();
        const auto& settings = resolvedConfig->defaultOutfit;
        const auto& defaults = Config::GetDefaultConfig().defaultOutfit;
        local = {};

        std::uint32_t unusable = 0;
        for (std::size_t slot = 0; slot < settings.armor.size(); ++slot)
        {
            local.armor[slot] = ResolveConfigured(settings.armor[slot], defaults.armor[slot],
                Config::GetOutfitArmorKey(slot), Config::GetOutfitArmorFilter(slot), unusable);
        }
        for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < settings.weapons.size();
            ++type)
        {
            local.weapons[type] = ResolveConfigured(settings.weapons[type], defaults.weapons[type],
                Config::GetOutfitWeaponKey(type), Config::GetOutfitWeaponFilter(type), unusable);
        }
        local.torch = ResolveConfigured(settings.torch, defaults.torch, "Torch", Config::kOutfitTorchFilter, unusable);
        local.arrow = ResolveConfigured(settings.arrow, defaults.arrow, "Arrow", Config::kOutfitArrowFilter, unusable);
        local.bolt = ResolveConfigured(settings.bolt, defaults.bolt, "Bolt", Config::kOutfitBoltFilter, unusable);

        logs::info("Default outfit initialized: unusableItems={}", unusable);
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
        if (Config::Get()->defaultOutfit == resolvedConfig->defaultOutfit)
        {
            resolvedConfigRevision = revision;
            return;
        }
        logs::info("Default outfit settings changed; resolving the configured items again");
        Initialize();
    }

    void Select(Multiplayer::PlayerEquipment& a_equipment)
    {
        auto& outfit = a_equipment.defaultOutfit;
        outfit = {};

        std::uint32_t standInSlots = 0;
        for (const auto& entry : a_equipment.armor)
        {
            if (!IsBaseGameItem(entry.form))
            {
                standInSlots |= entry.slotMask;
            }
        }
        for (const auto slot : kArmorSlotOrder)
        {
            const auto slotMask = Settings::kArmorSlotMasks[slot];
            if ((standInSlots & slotMask) != 0 && !local.armor[slot].IsEmpty())
            {
                outfit.armor.push_back(Multiplayer::EquipmentArmor{ .form = local.armor[slot], .slotMask = slotMask });
            }
        }

        const auto appendHand = [&outfit](const std::optional<Multiplayer::EquipmentHand>& a_hand) {
            if (!a_hand || a_hand->kind == EquipmentHandKind::kSpell || IsBaseGameItem(a_hand->form))
            {
                return;
            }
            const bool torch = a_hand->kind == EquipmentHandKind::kTorch;
            if (!torch && a_hand->weaponType >= local.weapons.size())
            {
                return;
            }
            const auto& form = torch ? local.torch : local.weapons[a_hand->weaponType];
            const bool duplicate
                = std::ranges::any_of(outfit.hands, [&a_hand](const Multiplayer::EquipmentHand& a_piece) {
                      return a_piece.kind == a_hand->kind && a_piece.weaponType == a_hand->weaponType;
                  });
            if (!form.IsEmpty() && !duplicate)
            {
                outfit.hands.push_back(
                    Multiplayer::EquipmentHand{ .form = form, .kind = a_hand->kind, .weaponType = a_hand->weaponType });
            }
        };
        appendHand(a_equipment.rightHand);
        appendHand(a_equipment.leftHand);

        if (!a_equipment.ammo.IsEmpty() && !IsBaseGameItem(a_equipment.ammo))
        {
            auto* ammo = FormLookup::ResolveForm<RE::TESAmmo>(a_equipment.ammo);
            outfit.ammo = ammo && ammo->IsBolt() ? local.bolt : local.arrow;
        }
    }

    void AppendArmor(const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentArmor& a_entry,
        std::uint32_t& a_occupiedSlots, std::vector<RE::TESObjectARMO*>& a_armor)
    {
        for (const auto& piece : a_outfit.armor)
        {
            const auto covered = a_entry.slotMask & piece.slotMask;
            if (covered == 0 || (covered & ~a_occupiedSlots) == 0)
            {
                continue;
            }
            auto* armor = FormLookup::ResolveForm<RE::TESObjectARMO>(piece.form);
            if (!armor)
            {
                continue;
            }
            // Equipping over an occupied slot would unequip the item shown there.
            const auto armorSlots = armor->GetSlotMask().underlying();
            if (armorSlots == 0 || (armorSlots & a_occupiedSlots) != 0)
            {
                continue;
            }
            a_occupiedSlots |= armorSlots;
            a_armor.push_back(armor);
        }
    }

    RE::TESBoundObject* FindHand(const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry)
    {
        if (a_entry.kind == EquipmentHandKind::kSpell)
        {
            return nullptr;
        }
        const auto piece = std::ranges::find_if(a_outfit.hands, [&a_entry](const Multiplayer::EquipmentHand& a_piece) {
            return a_piece.kind == a_entry.kind && a_piece.weaponType == a_entry.weaponType;
        });
        if (piece == a_outfit.hands.end())
        {
            return nullptr;
        }
        if (a_entry.kind == EquipmentHandKind::kTorch)
        {
            auto* light = FormLookup::ResolveForm<RE::TESObjectLIGH>(piece->form);
            return light && light->CanBeCarried() ? light : nullptr;
        }
        auto* weapon = FormLookup::ResolveForm<RE::TESObjectWEAP>(piece->form);
        return weapon && static_cast<std::uint8_t>(weapon->GetWeaponType()) == a_entry.weaponType ? weapon : nullptr;
    }

    RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit)
    {
        return FormLookup::ResolveForm<RE::TESAmmo>(a_outfit.ammo);
    }
}
