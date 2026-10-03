#include "config/SettingsSchema.h"
#include "protocol/Session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <system_error>

namespace Config
{
    namespace
    {
        using Multiplayer::FormReference;
        using KeyboardKey = RE::BSKeyboardDevice::Key;

        constexpr std::string_view kDefaultServerEndpoint = "https://8fa21dsv.lmfao.pp.ua";

        // Creation Kit slot names, indexed like DefaultOutfitSettings::kArmorSlotMasks.
        constexpr std::array<std::string_view, DefaultOutfitSettings::kArmorSlotCount> kArmorSlotNames{ "Head", "Body",
            "Hands", "Amulet", "Feet", "Shield", "Circlet", "Backpack" };
        // Creation Kit weapon animation types, indexed by RE::WEAPON_TYPE.
        constexpr std::array<std::string_view, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeNames{
            "HandToHandMelee", "OneHandSword", "OneHandDagger", "OneHandAxe", "OneHandMace", "TwoHandSword",
            "TwoHandAxe", "Bow", "Staff", "Crossbow"
        };

        constexpr std::array<BindableKey, 20> kBindableKeys{ {
            { "F1", KeyboardKey::kF1 },
            { "F2", KeyboardKey::kF2 },
            { "F3", KeyboardKey::kF3 },
            { "F4", KeyboardKey::kF4 },
            { "F5", KeyboardKey::kF5 },
            { "F6", KeyboardKey::kF6 },
            { "F7", KeyboardKey::kF7 },
            { "F8", KeyboardKey::kF8 },
            { "F9", KeyboardKey::kF9 },
            { "F10", KeyboardKey::kF10 },
            { "F11", KeyboardKey::kF11 },
            { "F12", KeyboardKey::kF12 },
            { "Insert", KeyboardKey::kInsert },
            { "Delete", KeyboardKey::kDelete },
            { "Home", KeyboardKey::kHome },
            { "End", KeyboardKey::kEnd },
            { "PageUp", KeyboardKey::kPageUp },
            { "PageDown", KeyboardKey::kPageDown },
            { "Pause", KeyboardKey::kPause },
            { "ScrollLock", KeyboardKey::kScrollLock },
        } };

        constexpr std::string_view kWhitespace = " \t\r\n";

        std::string_view Trim(std::string_view a_text)
        {
            const auto first = a_text.find_first_not_of(kWhitespace);
            if (first == std::string_view::npos)
            {
                return {};
            }
            return a_text.substr(first, a_text.find_last_not_of(kWhitespace) - first + 1);
        }

        bool EqualsIgnoreCase(std::string_view a_left, std::string_view a_right)
        {
            return std::ranges::equal(a_left, a_right, [](char a_first, char a_second) {
                return std::tolower(static_cast<unsigned char>(a_first))
                    == std::tolower(static_cast<unsigned char>(a_second));
            });
        }

        // Whitespace also rejects a trailing comment, which SimpleIni keeps in the value.
        bool IsValidServerEndpoint(std::string_view a_value)
        {
            return (a_value.starts_with("http://") || a_value.starts_with("https://"))
                && a_value.find_first_of(kWhitespace) == std::string_view::npos;
        }

        // Empty for not set yet.
        bool IsValidDisplayName(std::string_view a_value)
        {
            return a_value.empty() || Multiplayer::IsValidDisplayName(a_value);
        }

        void DescribeGeneral(GeneralSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("General", "");
            a_visitor.String("DisplayName", a_settings.displayName,
                "Shown to other players above your character and in chat.", IsValidDisplayName);
        }

        void DescribeNetwork(NetworkSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Network", "");
            a_visitor.String("ServerEndpoint", a_settings.serverEndpoint,
                "Server URL, starting with http:// or https://.", IsValidServerEndpoint);
            a_visitor.Bool("AutoConnect", a_settings.autoConnect,
                "");
        }

        void DescribeDefaultOutfit(DefaultOutfitSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("DefaultOutfit",
                "Player appearance and equipment shown to others in place of your\n"
                "modded assets they do not have.");
            const auto& defaults = GetDefaultConfig().defaultOutfit;

            a_visitor.Comment("Character");
            a_visitor.Form("Race", a_settings.race, defaults.race, kOutfitRaceFilter);
            a_visitor.Choice("Sex", a_settings.sex, "", DefaultOutfitSettings::kSexNames);

            a_visitor.Comment("Apparel");
            for (std::size_t slot = 0; slot < a_settings.armor.size(); ++slot)
            {
                a_visitor.Form(
                    GetOutfitArmorKey(slot), a_settings.armor[slot], defaults.armor[slot], GetOutfitArmorFilter(slot));
            }

            a_visitor.Comment("Weapons");
            for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < a_settings.weapons.size();
                ++type)
            {
                a_visitor.Form(GetOutfitWeaponKey(type), a_settings.weapons[type], defaults.weapons[type],
                    GetOutfitWeaponFilter(type));
            }

            a_visitor.Comment("Miscellaneous");
            a_visitor.Form("Arrow", a_settings.arrow, defaults.arrow, kOutfitArrowFilter);
            a_visitor.Form("Bolt", a_settings.bolt, defaults.bolt, kOutfitBoltFilter);
        }

        void DescribePerformance(PerformanceSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section(
                "Performance", "");
            a_visitor.Unsigned("ViewRadius", a_settings.viewRadius,
                "Game units within which other players are shown. Applies\n"
                "on the next connection.",
                100000, "Auto");
            a_visitor.Unsigned("SpawnsPerSecond", a_settings.spawnsPerSecond,
                "Remote players and mounts spawned per second at most. Many players joining or\n"
                "arriving at once then appear over a few seconds instead of in one long frame.\n"
                "0 spawns every due player at once.",
                1000, "Unlimited");
        }

        void DescribeUI(UISettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("UI", "");
            a_visitor.Key("SettingsKey", a_settings.settingsKey,
                "Opens the settings window.");
            a_visitor.Color("AccentColor", a_settings.accentColor,
                "Color that marks what can be selected or is selected in the settings window and\n"
                "the chat, as hexadecimal RRGGBB.",
                UISettings::kAccentSwatches);
            a_visitor.Choice("BorderColor", a_settings.frameColor,
                "Color of the frame around the settings window and the chat.",
                UISettings::kFrameColorNames);
            a_visitor.Percent("BorderThickness", a_settings.frameThickness,
                "Thickness of the frame around the settings window and the chat.",
                UISettings::kMinimumFrameThickness, UISettings::kMaximumFrameThickness);
            a_visitor.Bool("ItemPreview", a_settings.itemPreview,
                "Shows a 3D model of the hovered item in item lists.");
            a_visitor.Bool("StatusPanel", a_settings.statusPanel,
                "Displays the in-game debug status panel.");
            a_visitor.Layout("ChatWidth", a_settings.chatWidth,
                "Width of the chat in font sizes. Dragging the open chat's right edge sets it.",
                UISettings::kMinimumChatWidth, UISettings::kMaximumChatWidth);
            a_visitor.Layout("ChatLines", a_settings.chatLines,
                "Lines of history the open chat shows. Dragging its top edge sets it.",
                UISettings::kMinimumChatLines, UISettings::kMaximumChatLines);
        }

        PluginConfig MakeDefaultConfig()
        {
            PluginConfig config;
            config.network.serverEndpoint = kDefaultServerEndpoint;
            config.ui.settingsKey = KeyboardKey::kF10;

            auto& outfit = config.defaultOutfit;
            const auto skyrim = [](std::uint32_t a_localFormID) {
                return FormReference{ .plugin = "Skyrim.esm", .localFormID = a_localFormID };
            };
            const auto dawnguard = [](std::uint32_t a_localFormID) {
                return FormReference{ .plugin = "Dawnguard.esm", .localFormID = a_localFormID };
            };
            // NordRace.
            outfit.race = skyrim(0x013746);
            // Hide armor, a gold necklace, a circlet, and Anniversary Edition's basic
            // backpack, which vanilla lacks. The hide helmet also covers Circlet, and
            // Head comes first in kArmorSlotPrecedence, so the circlet only stands in
            // for circlet-only items. Indexed like kArmorSlotNames.
            outfit.armor = { skyrim(0x013913), skyrim(0x013911), skyrim(0x013912), skyrim(0x0877D5), skyrim(0x013910),
                skyrim(0x013914), skyrim(0x0166D6),
                FormReference{ .plugin = "ccFSVSSE001-Backpacks.esl", .localFormID = 0x000801 } };
            // Iron weapons, the hunting bow, an unenchanted staff, and the Dawnguard
            // crossbow.
            outfit.weapons[1] = skyrim(0x012EB7);
            outfit.weapons[2] = skyrim(0x01397E);
            outfit.weapons[3] = skyrim(0x013790);
            outfit.weapons[4] = skyrim(0x013982);
            outfit.weapons[5] = skyrim(0x01359D);
            outfit.weapons[6] = skyrim(0x013980);
            outfit.weapons[7] = skyrim(0x013985);
            outfit.weapons[8] = skyrim(0x0BE11F);
            outfit.weapons[9] = dawnguard(0x000801);
            outfit.arrow = skyrim(0x01397D);
            outfit.bolt = dawnguard(0x000BB3);
            return config;
        }

    }

    void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor)
    {
        DescribeGeneral(a_config.general, a_visitor);
        DescribeNetwork(a_config.network, a_visitor);
        DescribeDefaultOutfit(a_config.defaultOutfit, a_visitor);
        DescribePerformance(a_config.performance, a_visitor);
        DescribeUI(a_config.ui, a_visitor);
    }

    const PluginConfig& GetDefaultConfig()
    {
        static const PluginConfig defaults = MakeDefaultConfig();
        return defaults;
    }

    std::span<const BindableKey> GetBindableKeys()
    {
        return kBindableKeys;
    }

    std::optional<bool> ParseBool(std::string_view a_value)
    {
        for (const auto name : { "true", "yes", "on", "1" })
        {
            if (EqualsIgnoreCase(a_value, name))
            {
                return true;
            }
        }
        for (const auto name : { "false", "no", "off", "0" })
        {
            if (EqualsIgnoreCase(a_value, name))
            {
                return false;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> ParseUnsigned(std::string_view a_value)
    {
        std::uint32_t value = 0;
        const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value);
        if (a_value.empty() || error != std::errc{} || end != a_value.data() + a_value.size())
        {
            return std::nullopt;
        }
        return value;
    }

    std::optional<FormReference> ParseFormReference(std::string_view a_value)
    {
        const auto separator = a_value.find('|');
        if (separator == std::string_view::npos)
        {
            return std::nullopt;
        }

        const auto plugin = Trim(a_value.substr(0, separator));
        auto formID = Trim(a_value.substr(separator + 1));
        if (formID.starts_with("0x") || formID.starts_with("0X"))
        {
            formID.remove_prefix(2);
        }

        std::uint32_t localFormID = 0;
        const auto [end, error] = std::from_chars(formID.data(), formID.data() + formID.size(), localFormID, 16);
        if (formID.empty() || error != std::errc{} || end != formID.data() + formID.size())
        {
            return std::nullopt;
        }

        FormReference reference{ .plugin = std::string{ plugin }, .localFormID = localFormID };
        if (reference.IsEmpty() || !Multiplayer::IsValidFormReference(reference, false))
        {
            return std::nullopt;
        }
        return reference;
    }

    std::string FormatFormReference(const FormReference& a_reference)
    {
        return a_reference.IsEmpty() ? std::string{}
                                     : std::format("{}|0x{:06X}", a_reference.plugin, a_reference.localFormID);
    }

    std::optional<std::uint32_t> ParseChoice(std::string_view a_value, std::span<const std::string_view> a_options)
    {
        const auto option = std::ranges::find_if(
            a_options, [a_value](std::string_view a_option) { return EqualsIgnoreCase(a_option, a_value); });
        return option == a_options.end() ? std::nullopt
                                         : std::optional{ static_cast<std::uint32_t>(option - a_options.begin()) };
    }

    std::string_view FormatChoice(std::uint32_t a_value, std::span<const std::string_view> a_options)
    {
        return a_value < a_options.size() ? a_options[a_value] : std::string_view{};
    }

    std::optional<std::uint32_t> ParseColor(std::string_view a_value)
    {
        if (a_value.starts_with('#'))
        {
            a_value.remove_prefix(1);
        }
        constexpr std::size_t kDigits = 6;
        std::uint32_t color = 0;
        const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), color, 16);
        if (a_value.size() != kDigits || error != std::errc{} || end != a_value.data() + a_value.size())
        {
            return std::nullopt;
        }
        return color;
    }

    std::string FormatColor(std::uint32_t a_color)
    {
        return std::format("{:06X}", a_color & 0xFFFFFF);
    }

    std::optional<std::uint32_t> ParseKey(std::string_view a_value)
    {
        const auto key = std::ranges::find_if(
            kBindableKeys, [a_value](const BindableKey& a_key) { return EqualsIgnoreCase(a_key.name, a_value); });
        return key == kBindableKeys.end() ? std::nullopt : std::optional{ key->scanCode };
    }

    std::string_view FormatKey(std::uint32_t a_scanCode)
    {
        const auto key = std::ranges::find(kBindableKeys, a_scanCode, &BindableKey::scanCode);
        return key == kBindableKeys.end() ? std::string_view{} : key->name;
    }

    std::string_view GetOutfitArmorKey(std::size_t a_slotIndex)
    {
        return a_slotIndex < kArmorSlotNames.size() ? kArmorSlotNames[a_slotIndex] : "Unknown";
    }

    std::string_view GetOutfitWeaponKey(std::size_t a_weaponType)
    {
        return a_weaponType < kWeaponTypeNames.size() ? kWeaponTypeNames[a_weaponType] : "Unknown";
    }

    Forms::ItemFilter GetOutfitArmorFilter(std::size_t a_slotIndex)
    {
        if (a_slotIndex >= DefaultOutfitSettings::kArmorSlotCount)
        {
            return Forms::ItemFilter{ .kind = Forms::ItemKind::kArmor, .baseGameOnly = true };
        }
        // Only items whose main slot this is, so a hooded robe is a Body piece
        // and not a Head one.
        std::uint32_t excludedSlotMask = 0;
        for (const auto slot : DefaultOutfitSettings::kArmorSlotPrecedence)
        {
            if (slot == a_slotIndex)
            {
                break;
            }
            excludedSlotMask |= DefaultOutfitSettings::kArmorSlotMasks[slot];
        }
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kArmor,
            .slotMask = DefaultOutfitSettings::kArmorSlotMasks[a_slotIndex],
            .excludedSlotMask = excludedSlotMask,
            .baseGameOnly = true };
    }

    Forms::ItemFilter GetOutfitWeaponFilter(std::size_t a_weaponType)
    {
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kWeapon,
            .weaponType = static_cast<std::uint8_t>(a_weaponType),
            .baseGameOnly = true };
    }
}
