#include "config/SettingsSchema.h"
#include "protocol/Session.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <system_error>

namespace Config
{
    namespace
    {
        using Multiplayer::FormReference;
        using KeyboardKey = RE::BSKeyboardDevice::Key;

        // DIK_OEM_102, the key between left Shift and Z on ISO keyboards.
        constexpr std::uint32_t kIntlBackslash = 0x56;
        constexpr std::uint32_t kDefaultSettingsKey = KeyboardKey::kF10;
        constexpr std::uint32_t kDefaultChatKey = KeyboardKey::kEnter;

        constexpr std::string_view kDefaultServerEndpoint = PLAYERHUB_DEFAULT_SERVER_ENDPOINT;

        // Creation Kit slot names.
        constexpr std::array<std::string_view, DefaultOutfitSettings::kArmorSlotCount> kArmorSlotNames{ "Head", "Body",
            "Hands", "Amulet", "Feet", "Shield", "Circlet", "Backpack" };
        // Creation Kit weapon animation types, indexed by RE::WEAPON_TYPE.
        constexpr std::array<std::string_view, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeNames{
            "HandToHandMelee", "OneHandSword", "OneHandDagger", "OneHandAxe", "OneHandMace", "TwoHandSword",
            "TwoHandAxe", "Bow", "Staff", "Crossbow"
        };
        constexpr std::array<std::string_view, DefaultOutfitSettings::kArmorSlotCount> kArmorSlotLabels{ "Head", "Body",
            "Hands", "Amulet", "Feet", "Shield", "Circlet", "Backpack" };
        // TwoHandAxe covers warhammers too.
        constexpr std::array<std::string_view, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeLabels{
            "Unarmed", "Sword", "Dagger", "War axe", "Mace", "Greatsword", "Battleaxe or warhammer", "Bow", "Staff",
            "Crossbow"
        };

        struct BindableKey
        {
            std::string_view name;
            // Scan codes are physical positions; a label is what a US layout prints there.
            std::string_view label;
            std::uint32_t scanCode;
        };

        constexpr std::array<BindableKey, 91> kBindableKeys{ {
            { "F1", "F1", KeyboardKey::kF1 },
            { "F2", "F2", KeyboardKey::kF2 },
            { "F3", "F3", KeyboardKey::kF3 },
            { "F4", "F4", KeyboardKey::kF4 },
            { "F5", "F5", KeyboardKey::kF5 },
            { "F6", "F6", KeyboardKey::kF6 },
            { "F7", "F7", KeyboardKey::kF7 },
            { "F8", "F8", KeyboardKey::kF8 },
            { "F9", "F9", KeyboardKey::kF9 },
            { "F10", "F10", KeyboardKey::kF10 },
            { "F11", "F11", KeyboardKey::kF11 },
            { "F12", "F12", KeyboardKey::kF12 },
            { "A", "A", KeyboardKey::kA },
            { "B", "B", KeyboardKey::kB },
            { "C", "C", KeyboardKey::kC },
            { "D", "D", KeyboardKey::kD },
            { "E", "E", KeyboardKey::kE },
            { "F", "F", KeyboardKey::kF },
            { "G", "G", KeyboardKey::kG },
            { "H", "H", KeyboardKey::kH },
            { "I", "I", KeyboardKey::kI },
            { "J", "J", KeyboardKey::kJ },
            { "K", "K", KeyboardKey::kK },
            { "L", "L", KeyboardKey::kL },
            { "M", "M", KeyboardKey::kM },
            { "N", "N", KeyboardKey::kN },
            { "O", "O", KeyboardKey::kO },
            { "P", "P", KeyboardKey::kP },
            { "Q", "Q", KeyboardKey::kQ },
            { "R", "R", KeyboardKey::kR },
            { "S", "S", KeyboardKey::kS },
            { "T", "T", KeyboardKey::kT },
            { "U", "U", KeyboardKey::kU },
            { "V", "V", KeyboardKey::kV },
            { "W", "W", KeyboardKey::kW },
            { "X", "X", KeyboardKey::kX },
            { "Y", "Y", KeyboardKey::kY },
            { "Z", "Z", KeyboardKey::kZ },
            { "1", "1", KeyboardKey::kNum1 },
            { "2", "2", KeyboardKey::kNum2 },
            { "3", "3", KeyboardKey::kNum3 },
            { "4", "4", KeyboardKey::kNum4 },
            { "5", "5", KeyboardKey::kNum5 },
            { "6", "6", KeyboardKey::kNum6 },
            { "7", "7", KeyboardKey::kNum7 },
            { "8", "8", KeyboardKey::kNum8 },
            { "9", "9", KeyboardKey::kNum9 },
            { "0", "0", KeyboardKey::kNum0 },
            { "Minus", "-", KeyboardKey::kMinus },
            { "Equals", "=", KeyboardKey::kEquals },
            { "LeftBracket", "[", KeyboardKey::kBracketLeft },
            { "RightBracket", "]", KeyboardKey::kBracketRight },
            { "Semicolon", ";", KeyboardKey::kSemicolon },
            { "Apostrophe", "'", KeyboardKey::kApostrophe },
            { "Backslash", "\\", KeyboardKey::kBackslash },
            { "IntlBackslash", "\\ (ISO)", kIntlBackslash },
            { "Comma", ",", KeyboardKey::kComma },
            { "Period", ".", KeyboardKey::kPeriod },
            { "Slash", "/", KeyboardKey::kSlash },
            { "Space", "Space", KeyboardKey::kSpacebar },
            { "Enter", "Enter", KeyboardKey::kEnter },
            { "CapsLock", "Caps Lock", KeyboardKey::kCapsLock },
            { "Insert", "Insert", KeyboardKey::kInsert },
            { "Delete", "Delete", KeyboardKey::kDelete },
            { "Home", "Home", KeyboardKey::kHome },
            { "End", "End", KeyboardKey::kEnd },
            { "PageUp", "Page Up", KeyboardKey::kPageUp },
            { "PageDown", "Page Down", KeyboardKey::kPageDown },
            { "Up", "Up", KeyboardKey::kUp },
            { "Down", "Down", KeyboardKey::kDown },
            { "Left", "Left", KeyboardKey::kLeft },
            { "Right", "Right", KeyboardKey::kRight },
            { "Pause", "Pause", KeyboardKey::kPause },
            { "ScrollLock", "Scroll Lock", KeyboardKey::kScrollLock },
            { "NumLock", "Num Lock", KeyboardKey::kNumLock },
            { "Numpad0", "Num 0", KeyboardKey::kKP_0 },
            { "Numpad1", "Num 1", KeyboardKey::kKP_1 },
            { "Numpad2", "Num 2", KeyboardKey::kKP_2 },
            { "Numpad3", "Num 3", KeyboardKey::kKP_3 },
            { "Numpad4", "Num 4", KeyboardKey::kKP_4 },
            { "Numpad5", "Num 5", KeyboardKey::kKP_5 },
            { "Numpad6", "Num 6", KeyboardKey::kKP_6 },
            { "Numpad7", "Num 7", KeyboardKey::kKP_7 },
            { "Numpad8", "Num 8", KeyboardKey::kKP_8 },
            { "Numpad9", "Num 9", KeyboardKey::kKP_9 },
            { "NumpadDecimal", "Num .", KeyboardKey::kKP_Decimal },
            { "NumpadPlus", "Num +", KeyboardKey::kKP_Plus },
            { "NumpadMinus", "Num -", KeyboardKey::kKP_Subtract },
            { "NumpadMultiply", "Num *", KeyboardKey::kKP_Multiply },
            { "NumpadDivide", "Num /", KeyboardKey::kKP_Divide },
            { "NumpadEnter", "Num Enter", KeyboardKey::kKP_Enter },
        } };

        const BindableKey* FindKey(std::uint32_t a_scanCode)
        {
            const auto key = std::ranges::find(kBindableKeys, a_scanCode, &BindableKey::scanCode);
            return key == kBindableKeys.end() ? nullptr : &*key;
        }

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

        // SimpleIni keeps a trailing comment in the value.
        bool IsValidServerEndpoint(std::string_view a_value)
        {
            return a_value.empty()
                || ((a_value.starts_with("http://") || a_value.starts_with("https://"))
                    && a_value.find_first_of(kWhitespace) == std::string_view::npos);
        }

        bool IsValidDisplayName(std::string_view a_value)
        {
            return a_value.empty() || Multiplayer::IsValidDisplayName(a_value);
        }

        void DescribeConnection(ConnectionSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Connection", "");
            a_visitor.String("DisplayName", "Display name", a_settings.displayName,
                "Shown to other players above your character and in chat.", IsValidDisplayName);
            a_visitor.Bool("ConnectAutomatically", "Connect automatically", a_settings.autoConnect,
                "Connects to the server automatically after the game is loaded.");
            a_visitor.Bool("CustomServer", "Custom server", a_settings.customServer,
                "Connects to the server at the URL below instead of the official server.");
            a_visitor.ShowNextIf(a_settings.customServer);
            a_visitor.String("ServerEndpoint", "Server URL", a_settings.serverEndpoint,
                "Custom server URL, starting with http:// or https://. Used only while the\n"
                "custom server is on.",
                IsValidServerEndpoint);
        }

        void DescribeDefaultOutfit(DefaultOutfitSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("DefaultOutfit",
                "Player appearance and equipment shown to others in place of your\n"
                "modded assets they do not have.");
            const auto& defaults = GetDefaultConfig().defaultOutfit;

            a_visitor.Comment("Character");
            a_visitor.Form("Race", "Race", a_settings.race, defaults.race, kOutfitRaceFilter);
            a_visitor.Choice("Sex", "Sex", a_settings.sex, "", DefaultOutfitSettings::kSexNames);

            a_visitor.Comment("Apparel");
            for (std::size_t slot = 0; slot < a_settings.armor.size(); ++slot)
            {
                a_visitor.Form(GetOutfitArmorKey(slot), kArmorSlotLabels[slot], a_settings.armor[slot],
                    defaults.armor[slot], GetOutfitArmorFilter(slot));
            }

            a_visitor.Comment("Weapons");
            for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < a_settings.weapons.size();
                ++type)
            {
                a_visitor.Form(GetOutfitWeaponKey(type), kWeaponTypeLabels[type], a_settings.weapons[type],
                    defaults.weapons[type], GetOutfitWeaponFilter(type));
            }

            a_visitor.Comment("Miscellaneous");
            a_visitor.Form("Arrow", "Arrow", a_settings.arrow, defaults.arrow, kOutfitArrowFilter);
            a_visitor.Form("Bolt", "Bolt", a_settings.bolt, defaults.bolt, kOutfitBoltFilter);
        }

        void DescribeUI(UISettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("UI", "");
            a_visitor.Comment("Display");
            a_visitor.Percent("FontScale", "Font scale", a_settings.fontScale,
                "Size of the text in the settings window, the chat, and the rest of the\n"
                "multiplayer interface. Windows grow and shrink with it.",
                UISettings::kMinimumFontScale, UISettings::kMaximumFontScale, UISettings::kFontScaleStep);
            a_visitor.Color("AccentColor", "Accent color", a_settings.accentColor,
                "Color that marks what can be selected or is selected in the settings window and\n"
                "the chat, as hexadecimal RRGGBB.",
                UISettings::kAccentSwatches);
            a_visitor.Choice("BorderColor", "Border color", a_settings.frameColor,
                "Color of the frame around the settings window and the chat.",
                UISettings::kFrameColorNames);
            a_visitor.Percent("BorderThickness", "Border thickness", a_settings.frameThickness,
                "Thickness of the frame around the settings window and the chat.",
                UISettings::kMinimumFrameThickness, UISettings::kMaximumFrameThickness, 1);

            a_visitor.Comment("Players");
            a_visitor.Bool("Nameplates", "Nameplates", a_settings.nameplates,
                "Shows other players' names above their heads while they are in sight.");
            a_visitor.Unsigned("NameplateDistance", "Nameplate distance", a_settings.nameplateDistance,
                "Game units within which names are shown above players' heads, about 70 to a\n"
                "meter.",
                UISettings::kMaximumNameplateDistance, "", 1);

            a_visitor.Comment("Chat");
            a_visitor.Bool("CloseChatOnSend", "Close on send", a_settings.closeChatOnSend,
                "Closes the chat when Enter sends a line. Otherwise Enter only sends, and an\n"
                "empty line does nothing.");
            a_visitor.Layout("ChatWidth", a_settings.chatWidth,
                "Width of the chat in font sizes. Dragging the open chat's right edge sets it.",
                UISettings::kMinimumChatWidth, UISettings::kMaximumChatWidth);
            a_visitor.Layout("ChatLines", a_settings.chatLines,
                "Lines of history the open chat shows. Dragging its top edge sets it.",
                UISettings::kMinimumChatLines, UISettings::kMaximumChatLines);
            a_visitor.Percent("ChatPositionHorizontal", "Horizontal position", a_settings.chatPositionHorizontal,
                "Distance of the chat's left edge from the screen's left edge, as a share of the\n"
                "screen's width. The chat stays on the screen.",
                0, UISettings::kMaximumChatPosition, 1);
            a_visitor.Percent("ChatPositionVertical", "Vertical position", a_settings.chatPositionVertical,
                "Distance of the chat's bottom edge from the screen's bottom edge, as a share of\n"
                "the screen's height. The chat stays on the screen.",
                0, UISettings::kMaximumChatPosition, 1);

            a_visitor.Comment("Items");
            a_visitor.Bool("ItemPreview", "Item preview", a_settings.itemPreview,
                "Shows a 3D model of the hovered item in item lists.");
        }

        void DescribeControls(ControlsSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Controls", "");
            a_visitor.Key("OpenSettings", "Open settings", a_settings.settingsKey,
                "Opens and closes the settings window.", kDefaultSettingsKey);
            a_visitor.Key("OpenChat", "Open chat", a_settings.chatKey, "Opens the chat.", kDefaultChatKey);
            a_visitor.Key("ManagePlayers", "Manage players", a_settings.playersKey,
                "Opens the window that lists your friends and the players you muted or vanished.", 0);
            a_visitor.Key("AcceptTrade", "Accept trade", a_settings.tradeAcceptKey,
                "Accepts another player's trade offer while it waits for an answer.", 0);
            a_visitor.Key("DeclineTrade", "Decline trade", a_settings.tradeDeclineKey,
                "Declines another player's trade offer, or withdraws your own, while it waits\n"
                "for an answer.", 0);
        }

        void DescribeAdvanced(AdvancedSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Advanced", "");
            a_visitor.Comment("Performance");
            a_visitor.Unsigned("ViewRadius", "View radius", a_settings.viewRadius,
                "Game units within which other players are shown. Applies\n"
                "on the next connection.",
                AdvancedSettings::kMaximumViewRadius, "Auto", AdvancedSettings::kViewRadiusStep);
            a_visitor.Unsigned("SpawnsPerSecond", "Spawns per second", a_settings.spawnsPerSecond,
                "Remote players and mounts spawned per second at most. Many players joining or\n"
                "arriving at once then appear over a few seconds instead of in one long frame.\n"
                "0 spawns every due player at once.",
                AdvancedSettings::kMaximumSpawnsPerSecond, "Unlimited", 1);

            a_visitor.Comment("Diagnostics");
            a_visitor.Bool("StatusPanel", "Status panel", a_settings.statusPanel,
                "Shows the ping, network traffic, and the plugin's time per frame in the\n"
                "top-right corner.");
        }

        PluginConfig MakeDefaultConfig()
        {
            PluginConfig config;
            config.controls.settingsKey = kDefaultSettingsKey;
            config.controls.chatKey = kDefaultChatKey;
            config.controls.playersKey = KeyboardKey::kO;
            config.controls.tradeAcceptKey = KeyboardKey::kY;
            config.controls.tradeDeclineKey = KeyboardKey::kN;

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
            // backpack, which vanilla lacks.
            outfit.armor = { skyrim(0x013913), skyrim(0x013911), skyrim(0x013912), skyrim(0x0877D5), skyrim(0x013910),
                skyrim(0x013914), skyrim(0x0166D6),
                FormReference{ .plugin = "ccFSVSSE001-Backpacks.esl", .localFormID = 0x000801 } };
            // Iron weapons, the hunting bow, an unenchanted staff, and the Dawnguard crossbow.
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
        DescribeConnection(a_config.connection, a_visitor);
        DescribeDefaultOutfit(a_config.defaultOutfit, a_visitor);
        DescribeUI(a_config.ui, a_visitor);
        DescribeControls(a_config.controls, a_visitor);
        DescribeAdvanced(a_config.advanced, a_visitor);
    }

    const PluginConfig& GetDefaultConfig()
    {
        static const PluginConfig defaults = MakeDefaultConfig();
        return defaults;
    }

    std::string_view GetDefaultServerEndpoint()
    {
        return kDefaultServerEndpoint;
    }

    std::string_view GetServerEndpoint(const ConnectionSettings& a_settings)
    {
        return a_settings.customServer ? std::string_view{ a_settings.serverEndpoint } : kDefaultServerEndpoint;
    }

    std::optional<bool> ParseBool(std::string_view a_value)
    {
        for (const auto name : { "true", "yes", "on", "1" })
        {
            if (AsciiCase::Equals(a_value, name))
            {
                return true;
            }
        }
        for (const auto name : { "false", "no", "off", "0" })
        {
            if (AsciiCase::Equals(a_value, name))
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
            a_options, [a_value](std::string_view a_option) { return AsciiCase::Equals(a_option, a_value); });
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
            kBindableKeys, [a_value](const BindableKey& a_key) { return AsciiCase::Equals(a_key.name, a_value); });
        return key == kBindableKeys.end() ? std::nullopt : std::optional{ key->scanCode };
    }

    std::string_view FormatKey(std::uint32_t a_scanCode)
    {
        const auto* key = FindKey(a_scanCode);
        return key ? key->name : std::string_view{};
    }

    std::string_view FormatKeyLabel(std::uint32_t a_scanCode)
    {
        const auto* key = FindKey(a_scanCode);
        return key ? key->label : std::string_view{};
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
