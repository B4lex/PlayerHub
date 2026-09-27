#pragma once

#include "protocol/FormReference.h"

#include <array>
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
        // Any spell or scroll. Receivers show one placeholder spell, so it carries
        // no form.
        kSpell
    };

    // Biped slots synced for worn armor, as slot mask bits from slot 30: Head,
    // Hair, Body, Hands, Amulet, Feet, Shield, and Circlet. Armor covering none
    // of them is not synced.
    inline constexpr std::array<std::uint8_t, 8> kSyncedArmorSlots{ 0, 1, 2, 3, 5, 7, 9, 12 };
    inline constexpr std::uint32_t kSyncedArmorSlotMask = [] {
        std::uint32_t mask = 0;
        for (const auto slot : kSyncedArmorSlots)
        {
            mask |= 1U << slot;
        }
        return mask;
    }();

    // Worn armor, clothing, jewelry, or shield. The slot mask, limited to the
    // synced slots, selects a fallback when the form is unavailable on the
    // receiver.
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
        // RE::TESObjectWEAP::WEAPON_TYPE; zero for torches.
        std::uint8_t weaponType{ 0 };

        bool operator==(const EquipmentHand&) const = default;
    };

    // Reconstructable equipped-item state. Like appearance, it is published as
    // its own latest-wins state rather than in every transform checkpoint.
    struct PlayerEquipment
    {
        static constexpr std::size_t kMaximumArmor = 32;
        static constexpr std::uint8_t kMinimumWeaponType = 1;
        static constexpr std::uint8_t kMaximumWeaponType = 9;

        std::vector<EquipmentArmor> armor;
        std::optional<EquipmentHand> rightHand;
        std::optional<EquipmentHand> leftHand;
        // Equipped arrows or bolts. Empty form means none.
        FormReference ammo;
        // Source-local publication counter. Not part of the equipped state itself.
        std::uint64_t revision{ 0 };

        [[nodiscard]] bool HasSameItems(const PlayerEquipment& a_other) const
        {
            if (rightHand != a_other.rightHand || leftHand != a_other.leftHand || ammo != a_other.ammo
                || armor.size() != a_other.armor.size())
            {
                return false;
            }
            // Worn armor is a set; inventory iteration order is not meaningful.
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

    // Shared by capture and by the network parser so both sides enforce the same
    // bounds as the server.
    [[nodiscard]] inline bool IsValidEquipment(const PlayerEquipment& a_equipment)
    {
        if (a_equipment.revision == 0
            || a_equipment.revision > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            || a_equipment.armor.size() > PlayerEquipment::kMaximumArmor
            || !IsValidFormReference(a_equipment.ammo, true))
        {
            return false;
        }

        for (std::size_t i = 0; i < a_equipment.armor.size(); ++i)
        {
            const auto& entry = a_equipment.armor[i];
            if (!IsValidFormReference(entry.form, false) || entry.slotMask == 0
                || (entry.slotMask & ~kSyncedArmorSlotMask) != 0)
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
