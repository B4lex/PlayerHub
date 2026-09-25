#include "config/PluginConfig.h"

#include <SimpleIni.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace Config
{
    namespace
    {
        constexpr std::string_view kFileName = "skyrim-mp.ini";
        constexpr std::string_view kFallbackDirectory = "Data/SKSE/Plugins";
        constexpr std::string_view kDefaultServerEndpoint = "https://8fa21dsv.lmfao.pp.ua";
        constexpr std::size_t kMaximumModulePathLength = 32768;
        constexpr std::uint32_t kFirstArmorSlot = 30;

        // Creation Kit names of biped slots 30..43. Later slots have no vanilla
        // meaning and are named by number.
        constexpr std::array<std::string_view, 14> kArmorSlotNames{ "Head", "Hair", "Body", "Hands", "Forearms",
            "Amulet", "Ring", "Feet", "Calves", "Shield", "Tail", "LongHair", "Circlet", "Ears" };
        // Indexed by Multiplayer::EquipmentArmorType.
        constexpr std::array<std::string_view, EquipmentFallbackSettings::kArmorTypeCount> kArmorTypeNames{ "Light",
            "Heavy", "Clothing" };
        // Creation Kit weapon animation types, indexed by RE::WEAPON_TYPE.
        constexpr std::array<std::string_view, Multiplayer::PlayerEquipment::kMaximumWeaponType + 1> kWeaponTypeNames{
            "HandToHandMelee", "OneHandSword", "OneHandDagger", "OneHandAxe", "OneHandMace", "TwoHandSword",
            "TwoHandAxe", "Bow", "Staff", "Crossbow"
        };

        constexpr std::string_view kWhitespace = " \t\r\n";
        constexpr std::string_view kFileHeader
            = "Skyrim Multiplayer configuration. Written with default values when missing.\n"
              "Restart the game to apply changes. Removing a line restores its default.\n"
              "Comments must be on their own line.\n"
              "\n";

        using Multiplayer::FormReference;

        std::string ToUtf8(const std::filesystem::path& a_path)
        {
            const auto text = a_path.u8string();
            return { text.begin(), text.end() };
        }

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

        // `Plugin.esp|0x0012AB`: the defining plugin and the hexadecimal form ID local
        // to it. A full load-order form ID would not match across modpacks.
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

        // Whitespace also rejects a trailing comment, which SimpleIni keeps in the value.
        bool IsValidServerEndpoint(std::string_view a_value)
        {
            return (a_value.starts_with("http://") || a_value.starts_with("https://"))
                && a_value.find_first_of(kWhitespace) == std::string_view::npos;
        }

        // One description of every setting drives both reading the file and writing
        // the default one. A new option is a field in PluginConfig plus a line here.
        class SettingsVisitor
        {
        public:
            virtual ~SettingsVisitor() = default;

            virtual void Section(std::string_view a_name, std::string_view a_comment) = 0;
            virtual void Comment(std::string_view a_comment) = 0;
            virtual void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) = 0;
            virtual void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*a_isValid)(std::string_view)) = 0;
            // Grouped under a preceding Comment, so a form entry has none of its own.
            virtual void Form(std::string_view a_key, FormReference& a_value) = 0;
        };

        void DescribeNetwork(NetworkSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("Network", "Connection to the synchronization server.");
            a_visitor.String("ServerEndpoint", a_settings.serverEndpoint,
                "Server URL, starting with http:// or https://.", IsValidServerEndpoint);
        }

        void DescribeEquipmentFallback(EquipmentFallbackSettings& a_settings, SettingsVisitor& a_visitor)
        {
            a_visitor.Section("EquipmentFallback",
                "Items shown on remote players in place of equipment this game does not have,\n"
                "for example armor or weapons from a mod that is not installed here.\n"
                "Values are Plugin|FormID: the plugin that defines the item and the item's\n"
                "hexadecimal form ID without the load order prefix. An empty value means no fallback.");
            a_visitor.Bool("Enabled", a_settings.enabled, "Set to false to leave missing items off entirely.");

            a_visitor.Comment(
                "Armor, by biped slot: <Slot>.Light, <Slot>.Heavy, <Slot>.Clothing, or <Slot> for any\n"
                "armor type without its own value. A missing item gets the fallback of every slot it\n"
                "covers, unless that fallback would take a slot another shown item already uses.\n"
                "Slots: Head, Hair, Body, Hands, Forearms, Amulet, Ring, Feet, Calves, Shield, Tail,\n"
                "LongHair, Circlet, Ears, and Slot44 through Slot61. Vanilla helmets and hoods use Hair.");
            for (std::size_t slot = 0; slot < a_settings.armor.size(); ++slot)
            {
                auto& entry = a_settings.armor[slot];
                for (std::size_t type = 0; type < entry.byArmorType.size(); ++type)
                {
                    a_visitor.Form(GetArmorFallbackKey(slot, static_cast<Multiplayer::EquipmentArmorType>(type)),
                        entry.byArmorType[type]);
                }
                a_visitor.Form(GetArmorFallbackKey(slot, std::nullopt), entry.anyArmorType);
            }

            a_visitor.Comment(
                "Weapons, by weapon type. The fallback must have the same type. TwoHandAxe also covers\n"
                "warhammers.");
            for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < a_settings.weapons.size();
                ++type)
            {
                a_visitor.Form(GetWeaponFallbackKey(type), a_settings.weapons[type]);
            }

            a_visitor.Comment("Torch, and the ammunition shown with a bow (Arrow) or crossbow (Bolt).");
            a_visitor.Form("Torch", a_settings.torch);
            a_visitor.Form("Arrow", a_settings.arrow);
            a_visitor.Form("Bolt", a_settings.bolt);
        }

        void Describe(PluginConfig& a_config, SettingsVisitor& a_visitor)
        {
            DescribeNetwork(a_config.network, a_visitor);
            DescribeEquipmentFallback(a_config.equipmentFallback, a_visitor);
        }

        void SetDefaultArmorFallbacks(EquipmentFallbackSettings& a_settings)
        {
            struct DefaultArmor
            {
                std::uint32_t slot;
                std::uint32_t light;
                std::uint32_t heavy;
                std::uint32_t clothing;
                std::uint32_t any;
            };
            // Hide (light), iron (heavy), and farm clothes. The helmets and hood also
            // cover Circlet, and Hair is tried first, so the circlet only stands in
            // for missing circlet-only items.
            constexpr std::array<DefaultArmor, 9> kDefaults{ {
                { .slot = 30, .light = 0x013913, .heavy = 0x012E4D, .clothing = 0x0D3DE8, .any = 0 },
                { .slot = 31, .light = 0x013913, .heavy = 0x012E4D, .clothing = 0x0D3DE8, .any = 0 },
                { .slot = 32, .light = 0x013911, .heavy = 0x012E49, .clothing = 0x01BE1A, .any = 0 },
                { .slot = 33, .light = 0x013912, .heavy = 0x012E46, .clothing = 0, .any = 0 },
                { .slot = 35, .light = 0, .heavy = 0, .clothing = 0, .any = 0x0877D5 },
                { .slot = 36, .light = 0, .heavy = 0, .clothing = 0, .any = 0x01CF2B },
                { .slot = 37, .light = 0x013910, .heavy = 0x012E4B, .clothing = 0x01BE1B, .any = 0 },
                { .slot = 39, .light = 0x013914, .heavy = 0x012EB6, .clothing = 0, .any = 0 },
                { .slot = 42, .light = 0, .heavy = 0, .clothing = 0, .any = 0x0166D6 },
            } };

            const auto skyrim = [](std::uint32_t a_localFormID) {
                return a_localFormID == 0 ? FormReference{}
                                          : FormReference{ .plugin = "Skyrim.esm", .localFormID = a_localFormID };
            };
            for (const auto& entry : kDefaults)
            {
                auto& slot = a_settings.armor[entry.slot - kFirstArmorSlot];
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kLight)]
                    = skyrim(entry.light);
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kHeavy)]
                    = skyrim(entry.heavy);
                slot.byArmorType[static_cast<std::size_t>(Multiplayer::EquipmentArmorType::kClothing)]
                    = skyrim(entry.clothing);
                slot.anyArmorType = skyrim(entry.any);
            }
        }

        PluginConfig MakeDefaultConfig()
        {
            PluginConfig config;
            config.network.serverEndpoint = kDefaultServerEndpoint;

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

        class SettingsReader final : public SettingsVisitor
        {
        public:
            explicit SettingsReader(CSimpleIniA& a_file)
                : file(a_file)
            {
            }

            void Section(std::string_view a_name, std::string_view) override { section = a_name; }

            void Comment(std::string_view) override {}

            void Bool(std::string_view a_key, bool& a_value, std::string_view) override
            {
                const auto value = Take(a_key);
                if (!value)
                {
                    return;
                }
                if (const auto parsed = ParseBool(*value))
                {
                    a_value = *parsed;
                    return;
                }
                WarnInvalid(a_key, *value, a_value ? "true" : "false");
            }

            void String(std::string_view a_key, std::string& a_value, std::string_view,
                bool (*a_isValid)(std::string_view)) override
            {
                auto value = Take(a_key);
                if (!value)
                {
                    return;
                }
                if (a_isValid(*value))
                {
                    a_value = std::move(*value);
                    return;
                }
                WarnInvalid(a_key, *value, a_value);
            }

            void Form(std::string_view a_key, FormReference& a_value) override
            {
                const auto value = Take(a_key);
                if (!value)
                {
                    return;
                }
                if (value->empty())
                {
                    a_value = {};
                    return;
                }
                if (auto parsed = ParseFormReference(*value))
                {
                    a_value = std::move(*parsed);
                    return;
                }
                WarnInvalid(a_key, *value, FormatFormReference(a_value));
            }

            // Every key a setting reads has been removed, so what is left is unknown.
            void ReportUnknownEntries() const
            {
                CSimpleIniA::TNamesDepend sections;
                file.GetAllSections(sections);
                for (const auto& remainingSection : sections)
                {
                    CSimpleIniA::TNamesDepend keys;
                    file.GetAllKeys(remainingSection.pItem, keys);
                    for (const auto& key : keys)
                    {
                        logs::warn("Config {}: ignoring unknown setting [{}] {}", kFileName, remainingSection.pItem,
                            key.pItem);
                    }
                }
            }

        private:
            std::optional<std::string> Take(std::string_view a_key)
            {
                const std::string key{ a_key };
                const auto* value = file.GetValue(section.c_str(), key.c_str(), nullptr);
                if (!value)
                {
                    return std::nullopt;
                }
                std::string result{ value };
                file.Delete(section.c_str(), key.c_str());
                return result;
            }

            void WarnInvalid(std::string_view a_key, std::string_view a_value, std::string_view a_default) const
            {
                logs::warn("Config {}: invalid value `{}` for [{}] {}; using `{}`", kFileName, a_value, section, a_key,
                    a_default);
            }

            CSimpleIniA& file;
            std::string section;
        };

        class DefaultFileWriter final : public SettingsVisitor
        {
        public:
            DefaultFileWriter() { file.SetUnicode(true); }

            void Section(std::string_view a_name, std::string_view a_comment) override
            {
                section = a_name;
                const auto comment = TakeComment(a_comment);
                file.SetValue(section.c_str(), nullptr, nullptr, comment.empty() ? nullptr : comment.c_str());
            }

            void Comment(std::string_view a_comment) override { pendingComment = a_comment; }

            void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) override
            {
                Set(a_key, a_value ? "true" : "false", a_comment);
            }

            void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*)(std::string_view)) override
            {
                Set(a_key, a_value, a_comment);
            }

            void Form(std::string_view a_key, FormReference& a_value) override
            {
                // Unset forms stay out of the file; the group comment lists the keys.
                if (!a_value.IsEmpty())
                {
                    Set(a_key, FormatFormReference(a_value), {});
                }
            }

            [[nodiscard]] std::optional<std::string> Save() const
            {
                std::string text;
                if (file.Save(text, false) < 0)
                {
                    return std::nullopt;
                }
                return text;
            }

        private:
            void Set(std::string_view a_key, std::string_view a_value, std::string_view a_comment)
            {
                const auto comment = TakeComment(a_comment);
                file.SetValue(section.c_str(), std::string{ a_key }.c_str(), std::string{ a_value }.c_str(),
                    comment.empty() ? nullptr : comment.c_str());
            }

            // The pending group comment, then a_comment, as `;` lines.
            std::string TakeComment(std::string_view a_comment)
            {
                std::string text;
                for (auto part : { std::string_view{ pendingComment }, a_comment })
                {
                    while (!part.empty())
                    {
                        const auto end = part.find('\n');
                        const auto line = part.substr(0, end);
                        text += std::format("{};{}{}", text.empty() ? "" : "\n", line.empty() ? "" : " ", line);
                        part.remove_prefix(end == std::string_view::npos ? part.size() : end + 1);
                    }
                }
                pendingComment.clear();
                return text;
            }

            CSimpleIniA file;
            std::string section;
            // The file header leads the first section's comment.
            std::string pendingComment{ kFileHeader };
        };

        std::filesystem::path GetPluginDirectory()
        {
            std::wstring modulePath(260, L'\0');
            while (modulePath.size() <= kMaximumModulePathLength)
            {
                const auto length = REX::W32::GetModuleFileNameW(
                    REX::W32::GetCurrentModule(), modulePath.data(), static_cast<std::uint32_t>(modulePath.size()));
                if (length == 0)
                {
                    break;
                }
                // A length equal to the buffer size means the path was truncated.
                if (length < modulePath.size())
                {
                    modulePath.resize(length);
                    return std::filesystem::path{ modulePath }.parent_path();
                }
                modulePath.resize(modulePath.size() * 2);
            }
            logs::warn("Config could not resolve the plugin DLL path; using {}", kFallbackDirectory);
            return std::filesystem::path{ kFallbackDirectory };
        }

        std::optional<std::string> ReadFile(const std::filesystem::path& a_path)
        {
            std::ifstream input{ a_path, std::ios::binary };
            if (!input)
            {
                return std::nullopt;
            }
            return std::string{ std::istreambuf_iterator<char>{ input }, std::istreambuf_iterator<char>{} };
        }

        void WriteDefaultFile(const std::filesystem::path& a_path, PluginConfig a_defaults)
        {
            DefaultFileWriter writer;
            Describe(a_defaults, writer);
            const auto text = writer.Save();
            std::ofstream output;
            if (text)
            {
                output.open(a_path, std::ios::binary);
                output << *text;
                output.close();
            }
            if (!text || !output)
            {
                logs::warn("Config could not write the default {}; using defaults", ToUtf8(a_path));
                return;
            }
            logs::info("Config wrote the default {}", ToUtf8(a_path));
        }

        PluginConfig& GetStorage()
        {
            static PluginConfig config = MakeDefaultConfig();
            return config;
        }
    }

    void Load()
    {
        auto config = MakeDefaultConfig();
        const auto path = GetPluginDirectory() / kFileName;

        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
        {
            // Writing could truncate a file that exists but cannot be inspected.
            logs::warn("Config could not access {}: {}; using defaults", ToUtf8(path), error.message());
        }
        else if (!exists)
        {
            WriteDefaultFile(path, config);
        }
        else if (const auto text = ReadFile(path))
        {
            CSimpleIniA file;
            file.SetUnicode(true);
            if (file.LoadData(*text) < 0)
            {
                logs::warn("Config could not parse {}; using defaults", ToUtf8(path));
            }
            else
            {
                SettingsReader reader{ file };
                Describe(config, reader);
                reader.ReportUnknownEntries();
                logs::info("Config loaded {}", ToUtf8(path));
            }
        }
        else
        {
            logs::warn("Config could not read {}; using defaults", ToUtf8(path));
        }

        GetStorage() = std::move(config);
    }

    const PluginConfig& Get()
    {
        return GetStorage();
    }

    std::string GetArmorFallbackKey(std::size_t a_slotIndex, std::optional<Multiplayer::EquipmentArmorType> a_armorType)
    {
        auto key = a_slotIndex < kArmorSlotNames.size() ? std::string{ kArmorSlotNames[a_slotIndex] }
                                                        : std::format("Slot{}", kFirstArmorSlot + a_slotIndex);
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
}
