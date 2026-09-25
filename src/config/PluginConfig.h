#pragma once

#include "protocol/EquipmentState.h"
#include "protocol/FormReference.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Config
{
    struct NetworkSettings
    {
        std::string serverEndpoint;
    };

    // Local items shown in place of remote equipment this game cannot resolve. An
    // empty reference means no fallback.
    struct EquipmentFallbackSettings
    {
        // Biped slots 30..61, indexed from slot 30 like the slot mask bits.
        static constexpr std::size_t kArmorSlotCount = 32;
        static constexpr std::size_t kArmorTypeCount = 3;

        struct ArmorSlot
        {
            // Indexed by Multiplayer::EquipmentArmorType.
            std::array<Multiplayer::FormReference, kArmorTypeCount> byArmorType;
            // Used when the armor type's own entry is empty.
            Multiplayer::FormReference anyArmorType;
        };

        bool enabled{ true };
        std::array<ArmorSlot, kArmorSlotCount> armor;
        // Indexed by RE::WEAPON_TYPE; hand-to-hand is unused.
        std::array<Multiplayer::FormReference, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons;
        Multiplayer::FormReference torch;
        Multiplayer::FormReference arrow;
        Multiplayer::FormReference bolt;
    };

    struct PluginConfig
    {
        NetworkSettings network;
        EquipmentFallbackSettings equipmentFallback;
    };

    // Reads skyrim-mp.ini next to the plugin DLL, writing a default file when there
    // is none. Absent or invalid values keep their defaults. Call once at plugin
    // load, before anything reads the configuration.
    void Load();

    // Immutable after Load, so any thread may read it.
    [[nodiscard]] const PluginConfig& Get();

    // [EquipmentFallback] key names, so diagnostics name what to edit. No armor
    // type means the slot's any-type key.
    [[nodiscard]] std::string GetArmorFallbackKey(
        std::size_t a_slotIndex, std::optional<Multiplayer::EquipmentArmorType> a_armorType);
    [[nodiscard]] std::string_view GetWeaponFallbackKey(std::size_t a_weaponType);
}
