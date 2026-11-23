#include "config/SettingsSchema.h"
#include "protocol/Session.h"
#include "runtime/KeyState.h"
#include "text/AsciiCase.h"
#include "text/Text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

namespace Config
{
    namespace
    {
        using Multiplayer::FormReference;
        using KeyboardKey = RE::BSKeyboardDevice::Key;
        using MouseKey = RE::BSWin32MouseDevice::Key;
        using Text::Id;

        // DIK_OEM_102, the key between left Shift and Z on ISO keyboards.
        constexpr std::uint32_t kIntlBackslash = 0x56;
        constexpr std::uint32_t kDefaultSettingsKey = KeyboardKey::kF4;
        constexpr std::uint32_t kDefaultChatKey = KeyboardKey::kEnter;
        // Unbound in the vanilla PC controls.
        constexpr std::uint32_t kDefaultMicrophoneKey = KeyboardKey::kV;


        // Creation Kit slot names.
        constexpr std::array<std::string_view, DefaultLookSettings::kArmorSlotCount> kArmorSlotNames{ "Head", "Body",
            "Hands", "Amulet", "Feet", "Shield", "Circlet", "Backpack" };
        // Creation Kit weapon animation types, indexed by RE::WEAPON_TYPE.
        constexpr std::array<std::string_view, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeNames{
            "HandToHandMelee", "OneHandSword", "OneHandDagger", "OneHandAxe", "OneHandMace", "TwoHandSword",
            "TwoHandAxe", "Bow", "Staff", "Crossbow"
        };
        constexpr std::array<Id, DefaultLookSettings::kArmorSlotCount> kArmorSlotLabels{ Id::kLookHead,
            Id::kLookBody, Id::kLookHands, Id::kLookAmulet, Id::kLookFeet, Id::kLookShield,
            Id::kLookCirclet, Id::kLookBackpack };
        constexpr std::array<bool, DefaultLookSettings::kArmorSlotCount> kArmorSlotAllowsNone{ true, false, false,
            false, false, false, true, true };
        // Unarmed has no setting. TwoHandAxe covers warhammers too.
        constexpr std::array<Id, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeLabels{ Id::kNone,
            Id::kLookSword, Id::kLookDagger, Id::kLookWarAxe, Id::kLookMace, Id::kLookGreatsword,
            Id::kLookBattleaxe, Id::kLookBow, Id::kLookStaff, Id::kLookCrossbow };
        constexpr std::array<Id, UISettings::kFrameColorNames.size()> kFrameColorLabels{ Id::kUIBorderVanilla,
            Id::kUIBorderAccent };
        constexpr std::array<Id, VoiceSettings::kModeNames.size()> kVoiceModeLabels{ Id::kVoiceModePushToTalk,
            Id::kVoiceModeVoiceActivation };
        struct BindableKey
        {
            std::string_view name;
            // Scan codes are physical positions; a label is what a US layout prints there.
            std::string_view label;
            std::uint32_t code;
            // Replaces the label for keys named by a word.
            Id word{ Id::kNone };
        };

        constexpr std::array<BindableKey, 97> kBindableKeys{ {
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
            { "Space", "Space", KeyboardKey::kSpacebar, Id::kKeySpace },
            { "Enter", "Enter", KeyboardKey::kEnter, Id::kKeyEnter },
            { "CapsLock", "Caps Lock", KeyboardKey::kCapsLock, Id::kKeyCapsLock },
            { "Insert", "Insert", KeyboardKey::kInsert, Id::kKeyInsert },
            { "Delete", "Delete", KeyboardKey::kDelete, Id::kKeyDelete },
            { "Home", "Home", KeyboardKey::kHome, Id::kKeyHome },
            { "End", "End", KeyboardKey::kEnd, Id::kKeyEnd },
            { "PageUp", "Page Up", KeyboardKey::kPageUp, Id::kKeyPageUp },
            { "PageDown", "Page Down", KeyboardKey::kPageDown, Id::kKeyPageDown },
            { "Up", "Up", KeyboardKey::kUp, Id::kKeyUp },
            { "Down", "Down", KeyboardKey::kDown, Id::kKeyDown },
            { "Left", "Left", KeyboardKey::kLeft, Id::kKeyLeft },
            { "Right", "Right", KeyboardKey::kRight, Id::kKeyRight },
            { "Pause", "Pause", KeyboardKey::kPause, Id::kKeyPause },
            { "ScrollLock", "Scroll Lock", KeyboardKey::kScrollLock, Id::kKeyScrollLock },
            { "NumLock", "Num Lock", KeyboardKey::kNumLock, Id::kKeyNumLock },
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
            { "NumpadEnter", "Num Enter", KeyboardKey::kKP_Enter, Id::kKeyNumpadEnter },
            { "Mouse3", "Middle Mouse", KeyState::FromMouse(MouseKey::kMiddleButton), Id::kKeyMouseMiddle },
            { "Mouse4", "Mouse 4", KeyState::FromMouse(MouseKey::kButton3), Id::kKeyMouse4 },
            { "Mouse5", "Mouse 5", KeyState::FromMouse(MouseKey::kButton4), Id::kKeyMouse5 },
            { "Mouse6", "Mouse 6", KeyState::FromMouse(MouseKey::kButton5), Id::kKeyMouse6 },
            { "Mouse7", "Mouse 7", KeyState::FromMouse(MouseKey::kButton6), Id::kKeyMouse7 },
            { "Mouse8", "Mouse 8", KeyState::FromMouse(MouseKey::kButton7), Id::kKeyMouse8 },
        } };

        const BindableKey* FindKey(std::uint32_t a_key)
        {
            const auto key = std::ranges::find(kBindableKeys, a_key, &BindableKey::code);
            return key == kBindableKeys.end() ? nullptr : &*key;
        }

        constexpr std::uint16_t kDefaultServerPort = 14201;
        constexpr std::size_t kMaximumServerHostLength = 253;

        struct AddressParts
        {
            std::string_view host;
            std::uint16_t port{ kDefaultServerPort };
        };

        constexpr bool IsHostNameCharacter(char a_character)
        {
            return (a_character >= 'a' && a_character <= 'z') || (a_character >= 'A' && a_character <= 'Z')
                || (a_character >= '0' && a_character <= '9') || a_character == '-' || a_character == '.';
        }

        constexpr std::optional<AddressParts> ParseServerAddress(std::string_view a_address)
        {
            const auto colon = a_address.find(':');
            AddressParts result{ .host = a_address.substr(0, colon) };
            if (result.host.empty() || result.host.size() > kMaximumServerHostLength
                || !std::ranges::all_of(result.host, IsHostNameCharacter))
            {
                return std::nullopt;
            }
            if (colon == std::string_view::npos)
            {
                return result;
            }
            const auto port = a_address.substr(colon + 1);
            if (port.empty() || port.size() > 5)
            {
                return std::nullopt;
            }
            std::uint32_t number = 0;
            for (const char digit : port)
            {
                if (digit < '0' || digit > '9')
                {
                    return std::nullopt;
                }
                number = number * 10 + static_cast<std::uint32_t>(digit - '0');
            }
            if (number == 0 || number > 65535)
            {
                return std::nullopt;
            }
            result.port = static_cast<std::uint16_t>(number);
            return result;
        }

        constexpr std::string_view kDefaultServerAddress = PLAYERHUB_DEFAULT_SERVER_ADDRESS;
        static_assert(ParseServerAddress(kDefaultServerAddress).has_value(),
            "server_address must be a host or host:port");

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

        bool IsValidServerAddress(std::string_view a_value)
        {
            return a_value.empty() || ParseServerAddress(a_value).has_value();
        }

        bool IsValidDisplayName(std::string_view a_value)
        {
            return a_value.empty() || Multiplayer::IsValidDisplayName(a_value);
        }

        void DescribeConnection(ConnectionSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Connection", Id::kSectionConnection, Id::kNone);
            a_visitor.String("DisplayName", Id::kConnectionDisplayName, a_settings.displayName,
                Id::kConnectionDisplayNameComment, IsValidDisplayName);
            a_visitor.Bool("ConnectAutomatically", Id::kConnectionAutoConnect, a_settings.autoConnect,
                Id::kConnectionAutoConnectComment);
            a_visitor.Bool("CustomServer", Id::kConnectionCustomServer, a_settings.customServer,
                Id::kConnectionCustomServerComment);
            a_visitor.ShowNextIf(a_settings.customServer);
            a_visitor.String("ServerAddress", Id::kConnectionServerAddress, a_settings.serverAddress,
                Id::kConnectionServerAddressComment, IsValidServerAddress);
            a_visitor.ShowNextIf(a_settings.customServer);
            a_visitor.Bool(
                "ServerTls", Id::kConnectionServerTls, a_settings.serverTls, Id::kConnectionServerTlsComment);
        }

        void DescribeDefaultLook(DefaultLookSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("DefaultLook", Id::kSectionDefaultLook, Id::kLookComment);
            const auto& defaults = GetDefaultConfig().defaultLook;

            a_visitor.Bool("AlwaysUse", Id::kLookAlwaysUse, a_settings.alwaysUse, Id::kLookAlwaysUseComment);

            a_visitor.Comment(Id::kLookGroupCharacter);
            a_visitor.Form("Race", Id::kLookRace, a_settings.race, defaults.race, kLookRaceFilter, false);
            a_visitor.Preset("Preset", Id::kLookPreset, a_settings.preset, Id::kLookPresetComment);

            a_visitor.Comment(Id::kLookGroupApparel);
            for (std::size_t slot = 0; slot < a_settings.armor.size(); ++slot)
            {
                a_visitor.Form(GetLookArmorKey(slot), kArmorSlotLabels[slot], a_settings.armor[slot],
                    defaults.armor[slot], GetLookArmorFilter(slot), kArmorSlotAllowsNone[slot]);
            }

            a_visitor.Comment(Id::kLookGroupWeapons);
            for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < a_settings.weapons.size();
                ++type)
            {
                a_visitor.Form(GetLookWeaponKey(type), kWeaponTypeLabels[type], a_settings.weapons[type],
                    defaults.weapons[type], GetLookWeaponFilter(type), false);
            }

            a_visitor.Comment(Id::kLookGroupMiscellaneous);
            a_visitor.Form("Arrow", Id::kLookArrow, a_settings.arrow, defaults.arrow, kLookArrowFilter, false);
            a_visitor.Form("Bolt", Id::kLookBolt, a_settings.bolt, defaults.bolt, kLookBoltFilter, false);
        }

        void DescribeUI(UISettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("UI", Id::kSectionUI, Id::kNone);
            a_visitor.Comment(Id::kUIGroupDisplay);
            a_visitor.Language("Language", Id::kUILanguage, a_settings.language, Id::kUILanguageComment);
            a_visitor.Percent("Scale", Id::kUIScale, a_settings.scale, Id::kUIScaleComment, UISettings::kMinimumScale,
                UISettings::kMaximumScale, UISettings::kScaleStep);
            a_visitor.Color("AccentColor", Id::kUIAccentColor, a_settings.accentColor, Id::kUIAccentColorComment,
                UISettings::kAccentSwatches);
            a_visitor.Choice("BorderColor", Id::kUIBorderColor, a_settings.frameColor, Id::kUIBorderColorComment,
                UISettings::kFrameColorNames, kFrameColorLabels);
            a_visitor.Percent("BorderThickness", Id::kUIBorderThickness, a_settings.frameThickness,
                Id::kUIBorderThicknessComment, UISettings::kMinimumFrameThickness, UISettings::kMaximumFrameThickness,
                1);
            a_visitor.Percent("BackgroundOpacity", Id::kUIBackgroundOpacity, a_settings.backgroundOpacity,
                Id::kUIBackgroundOpacityComment, UISettings::kMinimumBackgroundOpacity,
                UISettings::kMaximumBackgroundOpacity, UISettings::kBackgroundOpacityStep);

            a_visitor.Comment(Id::kUIGroupPlayers);
            a_visitor.Bool("Nameplates", Id::kUINameplates, a_settings.nameplates, Id::kUINameplatesComment);
            a_visitor.Unsigned("NameplateDistance", Id::kUINameplateDistance, a_settings.nameplateDistance,
                Id::kUINameplateDistanceComment, UISettings::kMaximumNameplateDistance, Id::kNone, 1);

            a_visitor.Comment(Id::kUIGroupChat);
            a_visitor.Bool(
                "CloseChatOnSend", Id::kUICloseChatOnSend, a_settings.closeChatOnSend, Id::kUICloseChatOnSendComment);
            a_visitor.Layout("ChatWidth", a_settings.chatWidth,
                "Width of the chat in font sizes. Dragging the open chat's left or right edge sets it.",
                UISettings::kMinimumChatWidth, UISettings::kMaximumChatWidth);
            a_visitor.Layout("ChatLines", a_settings.chatLines,
                "Lines of history the open chat shows. Dragging its top or bottom edge sets it.",
                UISettings::kMinimumChatLines, UISettings::kMaximumChatLines);
            a_visitor.FinePercent("ChatPositionHorizontal", Id::kUIChatHorizontal, a_settings.chatPositionHorizontal,
                Id::kUIChatHorizontalComment, UISettings::kMaximumChatPosition);
            a_visitor.FinePercent("ChatPositionVertical", Id::kUIChatVertical, a_settings.chatPositionVertical,
                Id::kUIChatVerticalComment, UISettings::kMaximumChatPosition);

            a_visitor.Comment(Id::kUIGroupItems);
            a_visitor.Bool("ItemPreview", Id::kUIItemPreview, a_settings.itemPreview, Id::kUIItemPreviewComment);
        }

        void DescribeControls(ControlsSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Controls", Id::kSectionControls, Id::kNone);
            a_visitor.Key("OpenSettings", Id::kControlsOpenSettings, a_settings.settingsKey,
                Id::kControlsOpenSettingsComment, kDefaultSettingsKey);
            a_visitor.Key(
                "OpenChat", Id::kControlsOpenChat, a_settings.chatKey, Id::kControlsOpenChatComment, kDefaultChatKey);
            a_visitor.Key("ManagePlayers", Id::kControlsManagePlayers, a_settings.playersKey,
                Id::kControlsManagePlayersComment, 0);
            a_visitor.Key("AcceptTrade", Id::kControlsAcceptTrade, a_settings.tradeAcceptKey,
                Id::kControlsAcceptTradeComment, 0);
            a_visitor.Key("DeclineTrade", Id::kControlsDeclineTrade, a_settings.tradeDeclineKey,
                Id::kControlsDeclineTradeComment, 0);
            a_visitor.Key("Microphone", Id::kControlsMicrophone, a_settings.microphoneKey,
                Id::kControlsMicrophoneComment, 0);
        }

        void DescribeVoice(VoiceSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Voice", Id::kSectionVoice, Id::kVoiceComment);
            a_visitor.Comment(Id::kVoiceGroupMicrophone);
            // First, so hiding the rows below does not move the combo that hid them.
            a_visitor.AudioDevice("InputDevice", Id::kVoiceInputDevice, a_settings.inputDevice,
                Id::kVoiceInputDeviceComment, true);
            const bool microphone = a_settings.inputDevice != VoiceSettings::kNoInputDevice;
            a_visitor.ShowNextIf(microphone);
            a_visitor.Choice("Mode", Id::kVoiceMode, a_settings.mode, Id::kVoiceModeComment, VoiceSettings::kModeNames,
                kVoiceModeLabels);
            a_visitor.ShowNextIf(microphone);
            a_visitor.InputMeter(Id::kVoiceInputLevel, a_settings.activationLevel);
            a_visitor.ShowNextIf(microphone && a_settings.mode == VoiceSettings::kVoiceActivation);
            a_visitor.Percent("ActivationLevel", Id::kVoiceActivationLevel, a_settings.activationLevel,
                Id::kVoiceActivationLevelComment, 0, VoiceSettings::kMaximumActivationLevel, 1);
            a_visitor.ShowNextIf(microphone);
            a_visitor.Percent("InputVolume", Id::kVoiceInputVolume, a_settings.inputVolume, Id::kVoiceInputVolumeComment,
                VoiceSettings::kMinimumInputVolume, VoiceSettings::kMaximumInputVolume, VoiceSettings::kVolumeStep);
            a_visitor.ShowNextIf(microphone);
            a_visitor.Bool("NoiseSuppression", Id::kVoiceNoiseSuppression, a_settings.noiseSuppression,
                Id::kVoiceNoiseSuppressionComment);

            a_visitor.Comment(Id::kVoiceGroupPlayers);
            a_visitor.AudioDevice("OutputDevice", Id::kVoiceOutputDevice, a_settings.outputDevice,
                Id::kVoiceOutputDeviceComment, false);
            a_visitor.Percent("OutputVolume", Id::kVoiceOutputVolume, a_settings.outputVolume,
                Id::kVoiceOutputVolumeComment, 0, VoiceSettings::kMaximumVolume, VoiceSettings::kVolumeStep);

            a_visitor.Comment(Id::kVoiceGroupIndicator);
            a_visitor.Bool("Indicator", Id::kVoiceIndicator, a_settings.indicator, Id::kVoiceIndicatorComment);
            a_visitor.ShowNextIf(a_settings.indicator);
            a_visitor.FinePercent("IndicatorPositionHorizontal", Id::kVoiceIndicatorHorizontal,
                a_settings.indicatorPositionHorizontal, Id::kVoiceIndicatorHorizontalComment,
                VoiceSettings::kMaximumIndicatorPosition);
            a_visitor.ShowNextIf(a_settings.indicator);
            a_visitor.FinePercent("IndicatorPositionVertical", Id::kVoiceIndicatorVertical,
                a_settings.indicatorPositionVertical, Id::kVoiceIndicatorVerticalComment,
                VoiceSettings::kMaximumIndicatorPosition);
        }

        void DescribeGameplay(GameplaySettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Gameplay", Id::kSectionGameplay, Id::kNone);
            a_visitor.Comment(Id::kGameplayGroupEffects);
            a_visitor.Bool("ShowSpellEffects", Id::kGameplayShowSpellEffects, a_settings.showSpellEffects,
                Id::kGameplayShowSpellEffectsComment);
            a_visitor.Bool("ShowShoutEffects", Id::kGameplayShowShoutEffects, a_settings.showShoutEffects,
                Id::kGameplayShowShoutEffectsComment);

            a_visitor.Comment(Id::kGameplayGroupIsolation);
            a_visitor.Bool("IsolateSpellEffects", Id::kGameplayIsolateSpellEffects, a_settings.isolateSpellEffects,
                Id::kGameplayIsolateSpellEffectsComment);
            a_visitor.Bool("IsolateShoutEffects", Id::kGameplayIsolateShoutEffects, a_settings.isolateShoutEffects,
                Id::kGameplayIsolateShoutEffectsComment);
            a_visitor.Bool("DisableCollision", Id::kGameplayDisableCollision, a_settings.disableCollision,
                Id::kGameplayDisableCollisionComment);
        }

        void DescribeAdvanced(AdvancedSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Advanced", Id::kSectionAdvanced, Id::kNone);
            a_visitor.Comment(Id::kAdvancedGroupPerformance);
            a_visitor.Unsigned("ViewRadius", Id::kAdvancedViewRadius, a_settings.viewRadius,
                Id::kAdvancedViewRadiusComment, AdvancedSettings::kMaximumViewRadius, Id::kAdvancedViewRadiusAuto,
                AdvancedSettings::kViewRadiusStep);
            a_visitor.Unsigned("SpawnsPerSecond", Id::kAdvancedSpawnsPerSecond, a_settings.spawnsPerSecond,
                Id::kAdvancedSpawnsPerSecondComment, AdvancedSettings::kMaximumSpawnsPerSecond,
                Id::kAdvancedSpawnsUnlimited, 1);
            a_visitor.Unsigned("LightsLimit", Id::kAdvancedRemoteLights, a_settings.remoteLights,
                Id::kAdvancedRemoteLightsComment, AdvancedSettings::kMaximumRemoteLights, Id::kNone, 1);

            a_visitor.Comment(Id::kAdvancedGroupDiagnostics);
            a_visitor.Bool(
                "StatusPanel", Id::kAdvancedStatusPanel, a_settings.statusPanel, Id::kAdvancedStatusPanelComment);
        }

        PluginConfig MakeDefaultConfig()
        {
            PluginConfig config;
            config.controls.settingsKey = kDefaultSettingsKey;
            config.controls.chatKey = kDefaultChatKey;
            config.controls.playersKey = KeyboardKey::kO;
            config.controls.tradeAcceptKey = KeyboardKey::kY;
            config.controls.tradeDeclineKey = KeyboardKey::kN;
            config.controls.microphoneKey = kDefaultMicrophoneKey;

            auto& look = config.defaultLook;
            const auto skyrim = [](std::uint32_t a_localFormID) {
                return FormReference{ .plugin = "Skyrim.esm", .localFormID = a_localFormID };
            };
            const auto dawnguard = [](std::uint32_t a_localFormID) {
                return FormReference{ .plugin = "Dawnguard.esm", .localFormID = a_localFormID };
            };
            // NordRace.
            look.race = skyrim(0x013746);
            // Hide armor, a gold necklace, a circlet, and Anniversary Edition's basic
            // backpack, which vanilla lacks.
            look.armor = { skyrim(0x013913), skyrim(0x013911), skyrim(0x013912), skyrim(0x0877D5), skyrim(0x013910),
                skyrim(0x013914), skyrim(0x0166D6),
                FormReference{ .plugin = "ccFSVSSE001-Backpacks.esl", .localFormID = 0x000801 } };
            // Iron weapons, the hunting bow, an unenchanted staff, and the Dawnguard crossbow.
            look.weapons[1] = skyrim(0x012EB7);
            look.weapons[2] = skyrim(0x01397E);
            look.weapons[3] = skyrim(0x013790);
            look.weapons[4] = skyrim(0x013982);
            look.weapons[5] = skyrim(0x01359D);
            look.weapons[6] = skyrim(0x013980);
            look.weapons[7] = skyrim(0x013985);
            look.weapons[8] = skyrim(0x0BE11F);
            look.weapons[9] = dawnguard(0x000801);
            look.arrow = skyrim(0x01397D);
            look.bolt = dawnguard(0x000BB3);
            return config;
        }

    }

    void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor)
    {
        DescribeConnection(a_config.connection, a_visitor);
        DescribeDefaultLook(a_config.defaultLook, a_visitor);
        DescribeUI(a_config.ui, a_visitor);
        DescribeControls(a_config.controls, a_visitor);
        DescribeGameplay(a_config.gameplay, a_visitor);
        DescribeVoice(a_config.voice, a_visitor);
        DescribeAdvanced(a_config.advanced, a_visitor);
    }

    bool SettingsVisitor::IsValidAudioDeviceName(std::string_view a_value)
    {
        // SimpleIni would keep a trailing comment in the value, and a line break would end it.
        return a_value.size() <= VoiceSettings::kMaximumDeviceNameBytes
            && std::ranges::none_of(a_value, [](char a_character) {
                   return a_character == '\n' || a_character == '\r' || a_character == ';';
               });
    }

    bool SettingsVisitor::IsValidPreset(std::string_view a_value)
    {
        return a_value.empty() || DefaultLookSettings::IsValidPresetName(a_value);
    }

    const PluginConfig& GetDefaultConfig()
    {
        static const PluginConfig defaults = MakeDefaultConfig();
        return defaults;
    }

    ServerAddress GetDefaultServerAddress()
    {
        constexpr auto address = *ParseServerAddress(kDefaultServerAddress);
        return ServerAddress{
            .host = std::string{ address.host }, .port = address.port, .tls = PLAYERHUB_DEFAULT_SERVER_TLS
        };
    }

    std::optional<ServerAddress> GetServerAddress(const ConnectionSettings& a_settings)
    {
        if (!a_settings.customServer)
        {
            return GetDefaultServerAddress();
        }
        const auto address = ParseServerAddress(a_settings.serverAddress);
        if (!address)
        {
            return std::nullopt;
        }
        return ServerAddress{
            .host = std::string{ address->host }, .port = address->port, .tls = a_settings.serverTls
        };
    }

    bool HasServerAddress(const ConnectionSettings& a_settings)
    {
        return !a_settings.customServer || ParseServerAddress(a_settings.serverAddress).has_value();
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

    std::optional<float> ParseFloat(std::string_view a_value)
    {
        float value = 0.0F;
        const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), value);
        if (a_value.empty() || error != std::errc{} || end != a_value.data() + a_value.size() || !std::isfinite(value))
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
        return key == kBindableKeys.end() ? std::nullopt : std::optional{ key->code };
    }

    std::string_view FormatKey(std::uint32_t a_key)
    {
        const auto* key = FindKey(a_key);
        return key ? key->name : std::string_view{};
    }

    std::string_view FormatKeyLabel(std::uint32_t a_key)
    {
        const auto* key = FindKey(a_key);
        if (!key)
        {
            return {};
        }
        return key->word == Id::kNone ? key->label : Text::Get(key->word);
    }

    std::string_view GetLookArmorKey(std::size_t a_slotIndex)
    {
        return a_slotIndex < kArmorSlotNames.size() ? kArmorSlotNames[a_slotIndex] : "Unknown";
    }

    std::string_view GetLookWeaponKey(std::size_t a_weaponType)
    {
        return a_weaponType < kWeaponTypeNames.size() ? kWeaponTypeNames[a_weaponType] : "Unknown";
    }

    Forms::ItemFilter GetLookArmorFilter(std::size_t a_slotIndex)
    {
        if (a_slotIndex >= DefaultLookSettings::kArmorSlotCount)
        {
            return Forms::ItemFilter{ .kind = Forms::ItemKind::kArmor, .baseGameOnly = true };
        }
        std::uint32_t excludedSlotMask = 0;
        for (const auto slot : DefaultLookSettings::kArmorSlotPrecedence)
        {
            if (slot == a_slotIndex)
            {
                break;
            }
            excludedSlotMask |= DefaultLookSettings::kArmorSlotMasks[slot];
        }
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kArmor,
            .slotMask = DefaultLookSettings::kArmorSlotMasks[a_slotIndex],
            .excludedSlotMask = excludedSlotMask,
            .baseGameOnly = true };
    }

    Forms::ItemFilter GetLookWeaponFilter(std::size_t a_weaponType)
    {
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kWeapon,
            .weaponType = static_cast<std::uint8_t>(a_weaponType),
            .baseGameOnly = true };
    }
}
