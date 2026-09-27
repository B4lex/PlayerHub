#pragma once

#include "protocol/EquipmentState.h"
#include "protocol/FormReference.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Config
{
    struct NetworkSettings
    {
        std::string serverEndpoint;

        bool operator==(const NetworkSettings&) const = default;
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

            bool operator==(const ArmorSlot&) const = default;
        };

        bool enabled{ true };
        std::array<ArmorSlot, kArmorSlotCount> armor;
        // Indexed by RE::WEAPON_TYPE; hand-to-hand is unused.
        std::array<Multiplayer::FormReference, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons;
        Multiplayer::FormReference torch;
        Multiplayer::FormReference arrow;
        Multiplayer::FormReference bolt;

        bool operator==(const EquipmentFallbackSettings&) const = default;
    };

    struct PerformanceSettings
    {
        // Remote actors and mounts spawned per second at most. Zero is unlimited.
        std::uint32_t spawnsPerSecond{ 20 };

        bool operator==(const PerformanceSettings&) const = default;
    };

    struct UISettings
    {
        // DirectInput scan code that opens the settings window.
        std::uint32_t settingsKey{ 0 };

        bool operator==(const UISettings&) const = default;
    };

    struct PluginConfig
    {
        NetworkSettings network;
        EquipmentFallbackSettings equipmentFallback;
        PerformanceSettings performance;
        UISettings ui;

        bool operator==(const PluginConfig&) const = default;
    };

    // Reads skyrim-mp.ini next to the plugin DLL, writing a default file when there
    // is none. Absent or invalid values keep their defaults. Call once at plugin
    // load, before anything reads the configuration.
    void Load();

    // The live configuration. Safe from any thread; a snapshot never changes, so
    // hold it only as long as one consistent view is needed. Per-actor code
    // should read it once per pass rather than per actor.
    [[nodiscard]] std::shared_ptr<const PluginConfig> Get();

    // Advances with every Apply, so a consumer can notice a change without
    // comparing settings each pass. Safe from any thread.
    [[nodiscard]] std::uint64_t GetRevision();

    // Replaces the live configuration, then saves it to the file. Consumers pick
    // up the change on their own thread. Values must already be valid: the
    // settings window checks them with the same description the file reader
    // uses. Returns false if the file could not be written; the change still
    // applies for this session. Safe from any thread.
    [[nodiscard]] bool Apply(PluginConfig a_config);
}
