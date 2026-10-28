#include "sync/outfit/DefaultOutfit.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "forms/FormLookup.h"
#include "forms/ItemForms.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

namespace DefaultOutfit
{
    namespace
    {
        using Settings = Config::DefaultOutfitSettings;
        using Multiplayer::EquipmentHandKind;
        using Multiplayer::FormReference;

        const FormReference kTorch{ .plugin = "Skyrim.esm", .localFormID = 0x01D4EC };

        Settings local;
        std::shared_ptr<const Config::PluginConfig> resolvedConfig;
        std::uint64_t resolvedConfigRevision{ 0 };

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
            // The Anniversary Edition backpack default is missing from installs
            // without it.
            const bool useDefault = a_default != a_reference && FindUnusableReason(a_default, a_filter).empty();
            logs::warn("Default outfit item ignored [DefaultOutfit] {}: item={}|{:06X} reason={} using={}", a_label,
                a_reference.plugin, a_reference.localFormID, reason, useDefault ? "default" : "none");
            return useDefault ? a_default : FormReference{};
        }

        bool IsBaseGameItem(const FormReference& a_reference)
        {
            return Multiplayer::IsBaseGamePlugin(a_reference.plugin);
        }

        std::size_t GetSlotRank(std::uint32_t a_slotMask)
        {
            for (std::size_t rank = 0; rank < Settings::kArmorSlotCount; ++rank)
            {
                if ((a_slotMask & Settings::kArmorSlotMasks[Settings::kArmorSlotPrecedence[rank]]) != 0)
                {
                    return rank;
                }
            }
            return Settings::kArmorSlotCount;
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
        local.race = ResolveConfigured(settings.race, defaults.race, "Race", Config::kOutfitRaceFilter, unusable);
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
        local.arrow = ResolveConfigured(settings.arrow, defaults.arrow, "Arrow", Config::kOutfitArrowFilter, unusable);
        local.bolt = ResolveConfigured(settings.bolt, defaults.bolt, "Bolt", Config::kOutfitBoltFilter, unusable);

        logs::info("Default outfit initialized: unusableItems={}", unusable);
    }

    void Refresh()
    {
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
        logs::debug("Default outfit settings changed; resolving the configured items again");
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
        for (const auto slot : Settings::kArmorSlotPrecedence)
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
            const auto& form = torch ? kTorch : local.weapons[a_hand->weaponType];
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

    void SelectRace(Multiplayer::PlayerAppearance& a_appearance)
    {
        a_appearance.defaultRace.reset();
        if (!IsBaseGameItem(a_appearance.race) && !local.race.IsEmpty())
        {
            a_appearance.defaultRace = local.race;
        }
    }

    std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> SelectArmor(
        const Multiplayer::DefaultOutfit& a_outfit, std::span<RE::TESObjectARMO* const> a_resolved,
        std::uint32_t a_missingSlots, std::vector<RE::TESObjectARMO*>& a_armor)
    {
        struct Candidate
        {
            std::size_t rank{ 0 };
            std::size_t piece{ 0 };
            RE::TESObjectARMO* armor{ nullptr };
        };
        constexpr auto kNoPiece = (std::numeric_limits<std::size_t>::max)();

        std::vector<Candidate> candidates;
        candidates.reserve(a_resolved.size() + a_outfit.armor.size());
        for (auto* armor : a_resolved)
        {
            candidates.push_back(Candidate{ .rank = GetSlotRank(armor->GetSlotMask().underlying()),
                .piece = kNoPiece,
                .armor = armor });
        }
        const auto pieceCount = (std::min)(a_outfit.armor.size(), Multiplayer::DefaultOutfit::kMaximumArmor);
        for (std::size_t index = 0; index < pieceCount; ++index)
        {
            const auto& piece = a_outfit.armor[index];
            if ((piece.slotMask & a_missingSlots) == 0)
            {
                continue;
            }
            auto* armor = FormLookup::ResolveForm<RE::TESObjectARMO>(piece.form);
            if (armor && armor->GetSlotMask().underlying() != 0)
            {
                candidates.push_back(Candidate{ .rank = GetSlotRank(piece.slotMask), .piece = index, .armor = armor });
            }
        }
        std::ranges::stable_sort(candidates, [](const Candidate& a_left, const Candidate& a_right) {
            return a_left.rank != a_right.rank ? a_left.rank < a_right.rank
                                               : (a_left.piece == kNoPiece && a_right.piece != kNoPiece);
        });

        std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> standIns{};
        a_armor.clear();
        std::uint32_t occupiedSlots = 0;
        for (const auto& candidate : candidates)
        {
            const bool standIn = candidate.piece != kNoPiece;
            if (standIn && (a_outfit.armor[candidate.piece].slotMask & a_missingSlots & ~occupiedSlots) == 0)
            {
                continue;
            }
            // Equipping over an occupied slot would unequip the item taken there.
            const auto armorSlots = candidate.armor->GetSlotMask().underlying();
            if ((armorSlots & occupiedSlots) != 0)
            {
                continue;
            }
            occupiedSlots |= armorSlots;
            a_armor.push_back(candidate.armor);
            if (standIn)
            {
                standIns[candidate.piece] = candidate.armor;
            }
        }
        return standIns;
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
