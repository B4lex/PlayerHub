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
    struct ConnectionSettings
    {
        std::string displayName;
        bool autoConnect{ true };
        bool customServer{ false };
        std::string serverEndpoint;

        bool operator==(const ConnectionSettings&) const = default;
    };

    struct DefaultOutfitSettings
    {
        // Match RE::SEXES.
        static constexpr std::uint32_t kMale = 0;
        static constexpr std::uint32_t kFemale = 1;
        static constexpr std::array<std::string_view, 2> kSexNames{ "Male", "Female" };

        // Biped slot bits, bit 0 being slot 30. Anniversary Edition's backpacks and
        // most mods' use slot 47.
        static constexpr std::array<std::uint32_t, 8> kArmorSlotMasks{ (1U << 0) | (1U << 1), 1U << 2, 1U << 3, 1U << 5,
            1U << 7, 1U << 9, 1U << 12, 1U << 17 };
        static constexpr std::size_t kArmorSlotCount = kArmorSlotMasks.size();
        static constexpr std::array<std::size_t, kArmorSlotCount> kArmorSlotPrecedence{ 1, 0, 2, 4, 5, 3, 6, 7 };
        static_assert(kArmorSlotCount <= Multiplayer::DefaultOutfit::kMaximumArmor);

        Multiplayer::FormReference race;
        std::uint32_t sex{ kMale };
        std::array<Multiplayer::FormReference, kArmorSlotCount> armor;
        // Indexed by RE::WEAPON_TYPE.
        std::array<Multiplayer::FormReference, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> weapons;
        Multiplayer::FormReference arrow;
        Multiplayer::FormReference bolt;

        bool operator==(const DefaultOutfitSettings&) const = default;
    };

    struct AdvancedSettings
    {
        static constexpr std::uint32_t kMaximumViewRadius = 15000;
        static constexpr std::uint32_t kViewRadiusStep = 100;
        static constexpr std::uint32_t kMaximumSpawnsPerSecond = 100;

        std::uint32_t viewRadius{ 0 };
        std::uint32_t spawnsPerSecond{ 20 };
        bool statusPanel{ false };

        bool operator==(const AdvancedSettings&) const = default;
    };

    struct UISettings
    {
        static constexpr std::uint32_t kVanillaFrame = 0;
        static constexpr std::uint32_t kAccentFrame = 1;
        static constexpr std::array<std::string_view, 2> kFrameColorNames{ "Vanilla", "Accent" };
        static constexpr std::array<std::uint32_t, 8> kAccentSwatches{ 0xFF9000, 0x909090, 0xF2C14E, 0xE5484D, 0xE0559C, 0x9B6CF0,
            0x4F8EF7, 0x2EC4B6 };
        static constexpr std::uint32_t kMinimumFontScale = 80;
        static constexpr std::uint32_t kMaximumFontScale = 130;
        static constexpr std::uint32_t kFontScaleStep = 5;
        static constexpr std::uint32_t kMinimumFrameThickness = 30;
        static constexpr std::uint32_t kMaximumFrameThickness = 100;
        static constexpr std::uint32_t kMinimumChatWidth = 24;
        static constexpr std::uint32_t kMaximumChatWidth = 100;
        static constexpr std::uint32_t kMinimumChatLines = 9;
        static constexpr std::uint32_t kMaximumChatLines = 40;
        static constexpr std::uint32_t kMaximumNameplateDistance = 6000;

        std::uint32_t fontScale{ 100 };
        std::uint32_t accentColor{ kAccentSwatches[0] };
        std::uint32_t frameColor{ kVanillaFrame };
        std::uint32_t frameThickness{ 60 };
        bool itemPreview{ true };
        bool nameplates{ true };
        std::uint32_t nameplateDistance{ 2100 };
        bool closeChatOnSend{ true };
        std::uint32_t chatWidth{ 30 };
        std::uint32_t chatLines{ 10 };

        bool operator==(const UISettings&) const = default;
    };

    struct ControlsSettings
    {
        // DirectInput scan codes.
        std::uint32_t settingsKey{ 0 };
        std::uint32_t chatKey{ 0 };
        std::uint32_t playersKey{ 0 };
        std::uint32_t tradeAcceptKey{ 0 };
        std::uint32_t tradeDeclineKey{ 0 };

        bool operator==(const ControlsSettings&) const = default;
    };

    struct PluginConfig
    {
        ConnectionSettings connection;
        DefaultOutfitSettings defaultOutfit;
        UISettings ui;
        ControlsSettings controls;
        AdvancedSettings advanced;

        bool operator==(const PluginConfig&) const = default;
    };

    void Load();

    [[nodiscard]] std::filesystem::path GetDirectory();

    [[nodiscard]] std::shared_ptr<const PluginConfig> Get();

    [[nodiscard]] std::uint64_t GetRevision();

    [[nodiscard]] bool Apply(PluginConfig a_config);

    [[nodiscard]] std::string_view GetDefaultServerEndpoint();

    [[nodiscard]] std::string_view GetServerEndpoint(const ConnectionSettings& a_settings);
}
