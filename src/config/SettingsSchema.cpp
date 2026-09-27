#include "config/SettingsSchema.h"

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
        // Head, Hair, and Circlet. Helmets and hoods cover different ones of these
        // depending on the mod, so any of them fits a head slot's fallback.
        constexpr std::uint32_t kHeadSlotMask = (1U << 0) | (1U << 1) | (1U << 12);

        // Creation Kit slot names, indexed like Multiplayer::kSyncedArmorSlots.
        constexpr std::array<std::string_view, EquipmentFallbackSettings::kArmorSlotCount> kArmorSlotNames{ "Head",
            "Hair", "Body", "Hands", "Amulet", "Feet", "Shield", "Circlet" };
        // Jewelry looks the same whatever its armor type, so Amulet and Circlet
        // have only the any-type key.
        constexpr std::array<bool, EquipmentFallbackSettings::kArmorSlotCount> kTypedArmorSlots{ true, true, true,
            true, false, true, true, false };
        // Indexed by Multiplayer::EquipmentArmorType.
        constexpr std::array<std::string_view, EquipmentFallbackSettings::kArmorTypeCount> kArmorTypeNames{ "Light",
            "Heavy", "Clothing" };
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

        void DescribeNetwork(NetworkSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Network", "Connection to the synchronization server.");
            a_visitor.String("ServerEndpoint", a_settings.serverEndpoint,
                "Server URL, starting with http:// or https://.", IsValidServerEndpoint);
        }

        void DescribeEquipmentFallback(EquipmentFallbackSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("EquipmentFallback",
                "Equipment items shown on remote players in place of missing mod items.");
            a_visitor.Bool("Enabled", a_settings.enabled, "Set to false to leave missing items off entirely.");

            for (std::size_t slot = 0; slot < a_settings.armor.size(); ++slot)
            {
                auto& entry = a_settings.armor[slot];
                for (std::size_t type = 0; kTypedArmorSlots[slot] && type < entry.byArmorType.size(); ++type)
                {
                    const auto armorType = static_cast<Multiplayer::EquipmentArmorType>(type);
                    a_visitor.Form(GetArmorFallbackKey(slot, armorType), entry.byArmorType[type],
                        GetArmorFallbackFilter(slot, armorType));
                }
                a_visitor.Form(GetArmorFallbackKey(slot, std::nullopt), entry.anyArmorType,
                    GetArmorFallbackFilter(slot, std::nullopt));
            }

            a_visitor.Comment(
                "Weapons, by weapon type. The fallback must have the same type.");
            for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < a_settings.weapons.size();
                ++type)
            {
                a_visitor.Form(GetWeaponFallbackKey(type), a_settings.weapons[type], GetWeaponFallbackFilter(type));
            }

            a_visitor.Comment("Torch, and the ammunition shown with a bow (Arrow) or crossbow (Bolt).");
            a_visitor.Form("Torch", a_settings.torch, Forms::kTorchFilter);
            a_visitor.Form("Arrow", a_settings.arrow, Forms::kArrowFilter);
            a_visitor.Form("Bolt", a_settings.bolt, Forms::kBoltFilter);
        }

        void DescribePerformance(PerformanceSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Performance", "Limits that trade how quickly remote players appear for frame time.");
            a_visitor.Unsigned("SpawnsPerSecond", a_settings.spawnsPerSecond,
                "Remote players and mounts spawned per second at most. Many players joining or\n"
                "arriving at once then appear over a few seconds instead of in one long frame.\n"
                "0 spawns every due player at once.",
                1000);
        }

        void DescribeUI(UISettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("UI", "In-game overlay.");
            a_visitor.Key("SettingsKey", a_settings.settingsKey,
                "Opens the settings window.");
            a_visitor.Bool("ItemPreview", a_settings.itemPreview,
                "Shows a 3D model of the hovered item in item lists. Set to false if the preview\n"
                "shows wrong images or disturbs the game.");
        }

        void SetDefaultArmorFallbacks(EquipmentFallbackSettings& a_settings)
        {
            struct DefaultArmor
            {
                std::uint32_t light;
                std::uint32_t heavy;
                std::uint32_t clothing;
                std::uint32_t any;
            };
            // Hide (light), iron (heavy), and farm clothes. The helmets and hood also
            // cover Circlet, and Hair is tried first, so the circlet only stands in
            // for missing circlet-only items. Indexed like kArmorSlotNames.
            constexpr std::array<DefaultArmor, EquipmentFallbackSettings::kArmorSlotCount> kDefaults{ {
                { .light = 0x013913, .heavy = 0x012E4D, .clothing = 0x0D3DE8, .any = 0 },
                { .light = 0x013913, .heavy = 0x012E4D, .clothing = 0x0D3DE8, .any = 0 },
                { .light = 0x013911, .heavy = 0x012E49, .clothing = 0x01BE1A, .any = 0 },
                { .light = 0x013912, .heavy = 0x012E46, .clothing = 0, .any = 0 },
                { .light = 0, .heavy = 0, .clothing = 0, .any = 0x0877D5 },
                { .light = 0x013910, .heavy = 0x012E4B, .clothing = 0x01BE1B, .any = 0 },
                { .light = 0x013914, .heavy = 0x012EB6, .clothing = 0, .any = 0 },
                { .light = 0, .heavy = 0, .clothing = 0, .any = 0x0166D6 },
            } };

            const auto skyrim = [](std::uint32_t a_localFormID) {
                return a_localFormID == 0 ? FormReference{}
                                          : FormReference{ .plugin = "Skyrim.esm", .localFormID = a_localFormID };
            };
            for (std::size_t index = 0; index < kDefaults.size(); ++index)
            {
                const auto& entry = kDefaults[index];
                auto& slot = a_settings.armor[index];
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kLight)]
                    = skyrim(entry.light);
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kHeavy)]
                    = skyrim(entry.heavy);
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kClothing)]
                    = skyrim(entry.clothing);
                slot.anyArmorType = skyrim(entry.any);
            }
        }
    }

    void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor)
    {
        DescribeNetwork(a_config.network, a_visitor);
        DescribeEquipmentFallback(a_config.equipmentFallback, a_visitor);
        DescribePerformance(a_config.performance, a_visitor);
        DescribeUI(a_config.ui, a_visitor);
    }

    PluginConfig MakeDefaultConfig()
    {
        PluginConfig config;
        config.network.serverEndpoint = kDefaultServerEndpoint;
        config.ui.settingsKey = KeyboardKey::kF10;

        auto& fallback = config.equipmentFallback;
        SetDefaultArmorFallbacks(fallback);
        const auto skyrim = [](std::uint32_t a_localFormID) {
            return FormReference{ .plugin = "Skyrim.esm", .localFormID = a_localFormID };
        };
        const auto dawnguard = [](std::uint32_t a_localFormID) {
            return FormReference{ .plugin = "Dawnguard.esm", .localFormID = a_localFormID };
        };
        // Iron weapons, the hunting bow, an unenchanted staff, and the Dawnguard
        // crossbow.
        fallback.weapons[1] = skyrim(0x012EB7);
        fallback.weapons[2] = skyrim(0x01397E);
        fallback.weapons[3] = skyrim(0x013790);
        fallback.weapons[4] = skyrim(0x013982);
        fallback.weapons[5] = skyrim(0x01359D);
        fallback.weapons[6] = skyrim(0x013980);
        fallback.weapons[7] = skyrim(0x013985);
        fallback.weapons[8] = skyrim(0x0BE11F);
        fallback.weapons[9] = dawnguard(0x000801);
        fallback.torch = skyrim(0x01D4EC);
        fallback.arrow = skyrim(0x01397D);
        fallback.bolt = dawnguard(0x000BB3);
        return config;
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

    std::string GetArmorFallbackKey(std::size_t a_slotIndex, std::optional<Multiplayer::EquipmentArmorType> a_armorType)
    {
        std::string key{ a_slotIndex < kArmorSlotNames.size() ? kArmorSlotNames[a_slotIndex] : "Unknown" };
        if (a_armorType)
        {
            const auto type = static_cast<std::size_t>(*a_armorType);
            key += '.';
            key += type < kArmorTypeNames.size() ? kArmorTypeNames[type] : "Unknown";
        }
        return key;
    }

    std::string_view GetWeaponFallbackKey(std::size_t a_weaponType)
    {
        return a_weaponType < kWeaponTypeNames.size() ? kWeaponTypeNames[a_weaponType] : "Unknown";
    }

    Forms::ItemFilter GetArmorFallbackFilter(
        std::size_t a_slotIndex, std::optional<Multiplayer::EquipmentArmorType> a_armorType)
    {
        const auto slotBit = a_slotIndex < Multiplayer::kSyncedArmorSlots.size()
                                 ? 1U << Multiplayer::kSyncedArmorSlots[a_slotIndex]
                                 : 0U;
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kArmor,
            .slotMask = (slotBit & kHeadSlotMask) != 0 ? kHeadSlotMask : slotBit,
            .armorType = a_armorType };
    }

    Forms::ItemFilter GetWeaponFallbackFilter(std::size_t a_weaponType)
    {
        return Forms::ItemFilter{ .kind = Forms::ItemKind::kWeapon,
            .weaponType = static_cast<std::uint8_t>(a_weaponType) };
    }
}
