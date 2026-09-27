#pragma once

#include "protocol/EquipmentState.h"

#include <cstdint>
#include <optional>

namespace Forms
{
    enum class ItemKind : std::uint8_t
    {
        kArmor,
        kWeapon,
        kTorch,
        kAmmo
    };

    // What decides whether an item form can stand in for another.
    struct ItemTraits
    {
        ItemKind kind{ ItemKind::kArmor };
        // Armor: biped slots, bit 0 being slot 30.
        std::uint32_t slotMask{ 0 };
        Multiplayer::EquipmentArmorType armorType{ Multiplayer::EquipmentArmorType::kClothing };
        // Weapon: RE::WEAPON_TYPE, from PlayerEquipment::kMinimumWeaponType.
        std::uint8_t weaponType{ 0 };
        // Ammunition: a bolt rather than an arrow.
        bool bolt{ false };

        bool operator==(const ItemTraits&) const = default;
    };

    // The items a form setting or list accepts.
    struct ItemFilter
    {
        ItemKind kind{ ItemKind::kArmor };
        // Armor: the item covers at least one of these slots.
        std::uint32_t slotMask{ 0 };
        // Armor: nullopt accepts every armor type.
        std::optional<Multiplayer::EquipmentArmorType> armorType{};
        std::uint8_t weaponType{ 0 };
        bool bolt{ false };

        [[nodiscard]] constexpr bool Matches(const ItemTraits& a_traits) const noexcept
        {
            if (a_traits.kind != kind)
            {
                return false;
            }
            switch (kind)
            {
                case ItemKind::kArmor:
                    return (a_traits.slotMask & slotMask) != 0 && (!armorType || a_traits.armorType == *armorType);
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

    inline constexpr ItemFilter kTorchFilter{ .kind = ItemKind::kTorch };
    inline constexpr ItemFilter kArrowFilter{ .kind = ItemKind::kAmmo, .bolt = false };
    inline constexpr ItemFilter kBoltFilter{ .kind = ItemKind::kAmmo, .bolt = true };
}
