#pragma once

#include "protocol/EquipmentState.h"
#include "protocol/FormReference.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Config
{
    struct NetworkSettings
    {
        std::string serverEndpoint;
        // Whether each game starts connected, rather than waiting for Connect.
        bool autoConnect{ true };

        bool operator==(const NetworkSettings&) const = default;
    };

    // A base-game race and items, including Anniversary Edition content, peers show in place of the local player's
    // race and equipment from plugins they do not have. An empty reference means nothing is shown.
    struct DefaultOutfitSettings
    {
        // Indices of kSexNames, which match RE::SEXES.
        static constexpr std::uint32_t kMale = 0;
        static constexpr std::uint32_t kFemale = 1;
        static constexpr std::array<std::string_view, 2> kSexNames{ "Male", "Female" };

        // The biped slots, bit 0 being slot 30, that each armor piece stands in
        // for: Head (with Hair), Body, Hands, Amulet, Feet, Shield, Circlet, and
        // Backpack. Anniversary Edition's backpacks and most mods' use slot 47.
        static constexpr std::array<std::uint32_t, 8> kArmorSlotMasks{ (1U << 0) | (1U << 1), 1U << 2, 1U << 3, 1U << 5,
            1U << 7, 1U << 9, 1U << 12, 1U << 17 };
        static constexpr std::size_t kArmorSlotCount = kArmorSlotMasks.size();
        static_assert(kArmorSlotCount <= Multiplayer::DefaultOutfit::kMaximumArmor);

        Multiplayer::FormReference race;
        std::uint32_t sex{ kMale };
        // Indexed like kArmorSlotMasks.
        std::array<Multiplayer::FormReference, kArmorSlotCount> armor;
        // Indexed by RE::WEAPON_TYPE; hand-to-hand is unused.
        std::array<Multiplayer::FormReference, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons;
        Multiplayer::FormReference torch;
        Multiplayer::FormReference arrow;
        Multiplayer::FormReference bolt;

        bool operator==(const DefaultOutfitSettings&) const = default;
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
        bool itemPreview{ true };
        bool statusPanel{ true };

        bool operator==(const UISettings&) const = default;
    };

    struct PluginConfig
    {
        NetworkSettings network;
        DefaultOutfitSettings defaultOutfit;
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
