#pragma once

#include "protocol/FormReference.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace Multiplayer
{
    enum class EquipmentHandKind : std::uint8_t
    {
        kWeapon,
        kTorch,
        kSpell
    };

    struct EquipmentArmor
    {
        FormReference form;
        std::uint32_t slotMask{ 0 };

        bool operator==(const EquipmentArmor&) const = default;
    };

    struct EquipmentHand
    {
        FormReference form;
        EquipmentHandKind kind{ EquipmentHandKind::kWeapon };
        // RE::TESObjectWEAP::WEAPON_TYPE.
        std::uint8_t weaponType{ 0 };

        bool operator==(const EquipmentHand&) const = default;
    };

    struct DefaultOutfit
    {
        static constexpr std::size_t kMaximumArmor = 8;
        static constexpr std::size_t kMaximumHands = 2;

        std::vector<EquipmentArmor> armor;
        std::vector<EquipmentHand> hands;
        FormReference ammo;

        bool operator==(const DefaultOutfit&) const = default;
    };

    struct PlayerEquipment
    {
        static constexpr std::size_t kMaximumArmor = 32;
        static constexpr std::uint8_t kMinimumWeaponType = 1;
        static constexpr std::uint8_t kMaximumWeaponType = 9;

        std::vector<EquipmentArmor> armor;
        std::optional<EquipmentHand> rightHand;
        std::optional<EquipmentHand> leftHand;
        FormReference ammo;
        DefaultOutfit defaultOutfit;
        std::uint64_t revision{ 0 };

        [[nodiscard]] bool HasSameItems(const PlayerEquipment& a_other) const
        {
            if (rightHand != a_other.rightHand || leftHand != a_other.leftHand || ammo != a_other.ammo
                || defaultOutfit != a_other.defaultOutfit || armor.size() != a_other.armor.size())
            {
                return false;
            }
            for (const auto& entry : armor)
            {
                bool found = false;
                for (const auto& other : a_other.armor)
                {
                    if (entry == other)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return false;
                }
            }
            return true;
        }
    };

    [[nodiscard]] constexpr bool IsTwoHandedWeaponType(std::uint8_t a_weaponType) noexcept
    {
        // Two-handed sword, two-handed axe, bow, crossbow.
        return a_weaponType == 5 || a_weaponType == 6 || a_weaponType == 7 || a_weaponType == 9;
    }

    static_assert(IsTwoHandedWeaponType(7));
    static_assert(!IsTwoHandedWeaponType(8));

    [[nodiscard]] constexpr std::string_view GetEquipmentHandKindName(EquipmentHandKind a_kind) noexcept
    {
        switch (a_kind)
        {
            case EquipmentHandKind::kTorch:
                return "torch";
            case EquipmentHandKind::kSpell:
                return "spell";
            default:
                return "weapon";
        }
    }

    [[nodiscard]] inline bool IsValidEquipmentHand(const EquipmentHand& a_hand, bool a_leftHand)
    {
        if (a_hand.kind == EquipmentHandKind::kSpell)
        {
            return a_hand.form.IsEmpty() && a_hand.weaponType == 0;
        }
        if (!IsValidFormReference(a_hand.form, false))
        {
            return false;
        }
        if (a_hand.kind == EquipmentHandKind::kTorch)
        {
            return a_leftHand && a_hand.weaponType == 0;
        }
        return a_hand.weaponType >= PlayerEquipment::kMinimumWeaponType
            && a_hand.weaponType <= PlayerEquipment::kMaximumWeaponType
            && !(a_leftHand && IsTwoHandedWeaponType(a_hand.weaponType));
    }

    [[nodiscard]] inline bool IsValidDefaultOutfit(const DefaultOutfit& a_outfit)
    {
        const auto isBaseGameItem = [](const FormReference& a_form) {
            return !a_form.IsEmpty() && IsValidFormReference(a_form, false) && IsBaseGamePlugin(a_form.plugin);
        };
        if (a_outfit.armor.size() > DefaultOutfit::kMaximumArmor || a_outfit.hands.size() > DefaultOutfit::kMaximumHands
            || (!a_outfit.ammo.IsEmpty() && !isBaseGameItem(a_outfit.ammo)))
        {
            return false;
        }
        for (const auto& piece : a_outfit.armor)
        {
            if (!isBaseGameItem(piece.form) || piece.slotMask == 0)
            {
                return false;
            }
        }
        for (std::size_t i = 0; i < a_outfit.hands.size(); ++i)
        {
            const auto& hand = a_outfit.hands[i];
            if (!isBaseGameItem(hand.form))
            {
                return false;
            }
            const bool validKind = hand.kind == EquipmentHandKind::kTorch
                ? hand.weaponType == 0
                : hand.kind == EquipmentHandKind::kWeapon && hand.weaponType >= PlayerEquipment::kMinimumWeaponType
                    && hand.weaponType <= PlayerEquipment::kMaximumWeaponType;
            if (!validKind)
            {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (a_outfit.hands[j].kind == hand.kind && a_outfit.hands[j].weaponType == hand.weaponType)
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] inline bool IsValidEquipment(const PlayerEquipment& a_equipment)
    {
        if (a_equipment.revision == 0
            || a_equipment.revision > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            || a_equipment.armor.size() > PlayerEquipment::kMaximumArmor
            || !IsValidFormReference(a_equipment.ammo, true) || !IsValidDefaultOutfit(a_equipment.defaultOutfit))
        {
            return false;
        }

        for (std::size_t i = 0; i < a_equipment.armor.size(); ++i)
        {
            const auto& entry = a_equipment.armor[i];
            if (!IsValidFormReference(entry.form, false) || entry.slotMask == 0)
            {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (a_equipment.armor[j].form == entry.form)
                {
                    return false;
                }
            }
        }

        if (a_equipment.rightHand && !IsValidEquipmentHand(*a_equipment.rightHand, false))
        {
            return false;
        }
        if (a_equipment.leftHand && !IsValidEquipmentHand(*a_equipment.leftHand, true))
        {
            return false;
        }
        return !(a_equipment.rightHand && a_equipment.leftHand
            && a_equipment.rightHand->kind == EquipmentHandKind::kWeapon
            && IsTwoHandedWeaponType(a_equipment.rightHand->weaponType));
    }
}
