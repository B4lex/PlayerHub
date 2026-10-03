#pragma once

#include <cstdint>

namespace Forms
{
    enum class ItemKind : std::uint8_t
    {
        kArmor,
        kWeapon,
        kTorch,
        kAmmo,
        // A playable race. Not an item, but picked the same way.
        kRace
    };

    // Whether the inventory renderer can show the kind.
    [[nodiscard]] constexpr bool HasModel(ItemKind a_kind) noexcept
    {
        return a_kind != ItemKind::kRace;
    }

    // What decides whether an item form can stand in for another.
    struct ItemTraits
    {
        ItemKind kind{ ItemKind::kArmor };
        // Armor: biped slots, bit 0 being slot 30.
        std::uint32_t slotMask{ 0 };
        // Weapon: RE::WEAPON_TYPE, from PlayerEquipment::kMinimumWeaponType.
        std::uint8_t weaponType{ 0 };
        // Ammunition: a bolt rather than an arrow.
        bool bolt{ false };
        // Defined by a plugin Multiplayer::IsBaseGamePlugin accepts.
        bool baseGame{ false };

        bool operator==(const ItemTraits&) const = default;
    };

    // The items a form setting or list accepts.
    struct ItemFilter
    {
        ItemKind kind{ ItemKind::kArmor };
        // Armor: the item covers at least one of these slots.
        std::uint32_t slotMask{ 0 };
        // Armor: the item covers none of these slots.
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
