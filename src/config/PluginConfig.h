#pragma once

#include "protocol/EquipmentState.h"
#include "protocol/FormReference.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Config
{
    struct GeneralSettings
    {
        // Shown to other players over the local player's actor and in chat.
        // Empty until the player enters one; no connection is opened without it.
        std::string displayName;

        bool operator==(const GeneralSettings&) const = default;
    };

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
        // Indices into kArmorSlotMasks, most visible first: Body, Head, Hands,
        // Feet, Shield, Amulet, Circlet, Backpack. An item's main slot is the
        // first it covers. Receivers apply pieces in this order, and when the
        // pieces for one missing item overlap, the earlier slot wins.
        static constexpr std::array<std::size_t, kArmorSlotCount> kArmorSlotPrecedence{ 1, 0, 2, 4, 5, 3, 6, 7 };
        static_assert(kArmorSlotCount <= Multiplayer::DefaultOutfit::kMaximumArmor);

        Multiplayer::FormReference race;
        std::uint32_t sex{ kMale };
        // Indexed like kArmorSlotMasks.
        std::array<Multiplayer::FormReference, kArmorSlotCount> armor;
        // Indexed by RE::WEAPON_TYPE; hand-to-hand is unused.
        std::array<Multiplayer::FormReference, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons;
        Multiplayer::FormReference arrow;
        Multiplayer::FormReference bolt;

        bool operator==(const DefaultOutfitSettings&) const = default;
    };

    struct PerformanceSettings
    {
        // Game units within which peers are wanted; zero leaves it to the
        // server. Sent when connecting.
        std::uint32_t viewRadius{ 0 };
        // Remote actors and mounts spawned per second at most. Zero is unlimited.
        std::uint32_t spawnsPerSecond{ 20 };

        bool operator==(const PerformanceSettings&) const = default;
    };

    struct UISettings
    {
        // Indices of kFrameColorNames.
        static constexpr std::uint32_t kVanillaFrame = 0;
        static constexpr std::uint32_t kAccentFrame = 1;
        static constexpr std::array<std::string_view, 2> kFrameColorNames{ "Vanilla", "Accent" };
        // In percent of the size the game draws its menu frame at.
        static constexpr std::uint32_t kMinimumFrameThickness = 50;
        static constexpr std::uint32_t kMaximumFrameThickness = 100;
        // Of the open chat's lines: font sizes across, and how many show.
        static constexpr std::uint32_t kMinimumChatWidth = 24;
        static constexpr std::uint32_t kMaximumChatWidth = 100;
        static constexpr std::uint32_t kMinimumChatLines = 9;
        static constexpr std::uint32_t kMaximumChatLines = 40;

        // DirectInput scan code that opens the settings window.
        std::uint32_t settingsKey{ 0 };
        std::uint32_t frameColor{ kVanillaFrame };
        std::uint32_t frameThickness{ 70 };
        bool itemPreview{ true };
        bool statusPanel{ true };
        std::uint32_t chatWidth{ 30 };
        std::uint32_t chatLines{ 10 };

        bool operator==(const UISettings&) const = default;
    };

    struct PluginConfig
    {
        GeneralSettings general;
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

    // The directory skyrim-mp.ini is in, where the plugin keeps its other files.
    // Empty before Load. Safe from any thread.
    [[nodiscard]] std::filesystem::path GetDirectory();

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
