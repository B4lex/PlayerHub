#pragma once

#include <cstdint>

namespace Forms
{
    enum class ItemKind : std::uint8_t
    {
        kArmor,
        kWeapon,
        kAmmo,
        kRace
    };

    [[nodiscard]] constexpr bool HasModel(ItemKind a_kind) noexcept
    {
        return a_kind != ItemKind::kRace;
    }

    struct ItemTraits
    {
        ItemKind kind{ ItemKind::kArmor };
        // Biped slots, bit 0 being slot 30.
        std::uint32_t slotMask{ 0 };
        // RE::WEAPON_TYPE.
        std::uint8_t weaponType{ 0 };
        bool bolt{ false };
        bool baseGame{ false };

        bool operator==(const ItemTraits&) const = default;
    };

    struct ItemFilter
    {
        ItemKind kind{ ItemKind::kArmor };
        std::uint32_t slotMask{ 0 };
        std::uint32_t excludedSlotMask{ 0 };
        std::uint8_t weaponType{ 0 };
        bool bolt{ false };
        bool baseGameOnly{ false };

        [[nodiscard]] constexpr bool Matches(const ItemTraits& a_traits) const noexcept
        {
            if (a_traits.kind != kind || (baseGameOnly && !a_traits.baseGame))
            {
                return false;
            }
            switch (kind)
            {
                case ItemKind::kArmor:
                    return (a_traits.slotMask & slotMask) != 0 && (a_traits.slotMask & excludedSlotMask) == 0;
                case ItemKind::kWeapon:
                    return a_traits.weaponType == weaponType;
                case ItemKind::kAmmo:
                    return a_traits.bolt == bolt;
                default:
                    return true;
            }
        }

        bool operator==(const ItemFilter&) const = default;
    };

}
