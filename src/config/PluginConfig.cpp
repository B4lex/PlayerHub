#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"

#include <SimpleIni.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Config
{
    namespace
    {
        constexpr std::string_view kFileName = "skyrim-mp.ini";
        constexpr std::string_view kTemporaryExtension = ".tmp";
        constexpr std::string_view kFallbackDirectory = "Data/SKSE/Plugins";
        constexpr std::size_t kMaximumModulePathLength = 32768;

        constexpr std::string_view kFileHeader
            = "Skyrim Multiplayer configuration. Written with default values when missing.\n"
              "Edits here apply on the next start; the in-game settings window ([UI] SettingsKey)\n"
              "applies changes at once and saves them here. Removing a line restores its default.\n"
              "Comments must be on their own line.\n"
              "\n";

        using Multiplayer::FormReference;

        std::string ToUtf8(const std::filesystem::path& a_path)
        {
            const auto text = a_path.u8string();
            return { text.begin(), text.end() };
        }

        // Each line of the parts as a `;` comment line.
        std::string FormatComment(std::initializer_list<std::string_view> a_parts)
        {
            std::string text;
            for (auto part : a_parts)
            {
                while (!part.empty())
                {
                    const auto end = part.find('\n');
                    const auto line = part.substr(0, end);
                    text += std::format("{};{}{}", text.empty() ? "" : "\n", line.empty() ? "" : " ", line);
                    part.remove_prefix(end == std::string_view::npos ? part.size() : end + 1);
                }
            }
            return text;
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

            void Unsigned(
                std::string_view a_key, std::uint32_t& a_value, std::string_view, std::uint32_t a_maximum) override
            {
                const auto value = Take(a_key);
                if (!value)
                {
                    return;
                }
                if (const auto parsed = ParseUnsigned(*value); parsed && *parsed <= a_maximum)
                {
                    a_value = *parsed;
                    return;
                }
                WarnInvalid(a_key, *value, std::format("{}", a_value));
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

            void Form(std::string_view a_key, FormReference& a_value, const Forms::ItemFilter&) override
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

            void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view) override
            {
                const auto value = Take(a_key);
                if (!value)
                {
                    return;
                }
                if (const auto parsed = ParseKey(*value))
                {
                    a_value = *parsed;
                    return;
                }
                WarnInvalid(a_key, *value, FormatKey(a_value));
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

        // Visits every setting as the text the file stores for it.
        class TextVisitor : public SettingsVisitor
        {
        public:
            void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) override
            {
                Text(a_key, a_value ? "true" : "false", a_comment);
            }

            void Unsigned(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
                std::uint32_t) override
            {
                Text(a_key, std::format("{}", a_value), a_comment);
            }

            void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*)(std::string_view)) override
            {
                Text(a_key, a_value, a_comment);
            }

            void Form(std::string_view a_key, FormReference& a_value, const Forms::ItemFilter&) override
            {
                Text(a_key, FormatFormReference(a_value), {});
            }

            void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) override
            {
                Text(a_key, FormatKey(a_value), a_comment);
            }

        protected:
            virtual void Text(std::string_view a_key, std::string_view a_value, std::string_view a_comment) = 0;
        };

        class DefaultFileWriter final : public TextVisitor
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

            [[nodiscard]] std::optional<std::string> Save() const
            {
                std::string text;
                if (file.Save(text, false) < 0)
                {
                    return std::nullopt;
                }
                return text;
            }

        protected:
            void Text(std::string_view a_key, std::string_view a_value, std::string_view a_comment) override
            {
                // Unset forms stay out of the file; the group comment lists the keys.
                if (a_value.empty())
                {
                    return;
                }
                const auto comment = TakeComment(a_comment);
                file.SetValue(section.c_str(), std::string{ a_key }.c_str(), std::string{ a_value }.c_str(),
                    comment.empty() ? nullptr : comment.c_str());
            }

        private:
            // The pending group comment, then a_comment.
            std::string TakeComment(std::string_view a_comment)
            {
                auto text = FormatComment({ pendingComment, a_comment });
                pendingComment.clear();
                return text;
            }

            CSimpleIniA file;
            std::string section;
            // The file header leads the first section's comment.
            std::string pendingComment{ kFileHeader };
        };

        // Every setting's text, in description order.
        class ValueCollector final : public TextVisitor
        {
        public:
            void Section(std::string_view, std::string_view) override {}
            void Comment(std::string_view) override {}

            std::vector<std::string> values;

        protected:
            void Text(std::string_view, std::string_view a_value, std::string_view) override
            {
                values.emplace_back(a_value);
            }
        };

        // Writes settings into a loaded file, keeping its comments, its order, and
        // entries no setting reads. A setting that is absent from the file and at
        // its default stays absent, so a removed line keeps meaning "default".
        class FileUpdater final : public TextVisitor
        {
        public:
            FileUpdater(CSimpleIniA& a_file, std::span<const std::string> a_defaults)
                : file(a_file)
                , defaults(a_defaults)
            {
            }

            void Section(std::string_view a_name, std::string_view a_comment) override
            {
                section = a_name;
                sectionComment = a_comment;
            }

            void Comment(std::string_view) override {}

        protected:
            void Text(std::string_view a_key, std::string_view a_value, std::string_view a_comment) override
            {
                const auto& defaultValue = defaults[next++];
                const std::string key{ a_key };
                if (a_value == defaultValue && !file.KeyExists(section.c_str(), key.c_str()))
                {
                    return;
                }
                // SimpleIni only uses a comment for a section or key it adds.
                if (!file.SectionExists(section.c_str()))
                {
                    const auto comment = FormatComment({ sectionComment });
                    file.SetValue(section.c_str(), nullptr, nullptr, comment.empty() ? nullptr : comment.c_str());
                }
                const auto comment = FormatComment({ a_comment });
                file.SetValue(section.c_str(), key.c_str(), std::string{ a_value }.c_str(),
                    comment.empty() ? nullptr : comment.c_str());
            }

        private:
            CSimpleIniA& file;
            std::span<const std::string> defaults;
            std::size_t next{ 0 };
            std::string section;
            std::string_view sectionComment;
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

        // Through a temporary file, so a failed write never leaves a truncated file.
        bool WriteFile(const std::filesystem::path& a_path, std::string_view a_text)
        {
            auto temporaryPath = a_path;
            temporaryPath += kTemporaryExtension;
            {
                std::ofstream output{ temporaryPath, std::ios::binary | std::ios::trunc };
                output.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
                output.close();
                if (!output)
                {
                    return false;
                }
            }

            std::error_code error;
            std::filesystem::rename(temporaryPath, a_path, error);
            if (error)
            {
                logs::warn("Config could not replace {}: {}", ToUtf8(a_path), error.message());
                std::filesystem::remove(temporaryPath, error);
                return false;
            }
            return true;
        }

        std::optional<std::string> MakeDefaultFileText()
        {
            auto defaults = MakeDefaultConfig();
            DefaultFileWriter writer;
            Describe(defaults, writer);
            return writer.Save();
        }

        void WriteDefaultFile(const std::filesystem::path& a_path)
        {
            const auto text = MakeDefaultFileText();
            if (!text || !WriteFile(a_path, *text))
            {
                logs::warn("Config could not write the default {}; using defaults", ToUtf8(a_path));
                return;
            }
            logs::info("Config wrote the default {}", ToUtf8(a_path));
        }

        bool SaveFile(const std::filesystem::path& a_path, PluginConfig a_config)
        {
            CSimpleIniA file;
            file.SetUnicode(true);
            // A missing or unreadable file is recreated from the commented default.
            auto text = ReadFile(a_path);
            if (!text || file.LoadData(*text) < 0)
            {
                file.Reset();
                text = MakeDefaultFileText();
                if (!text || file.LoadData(*text) < 0)
                {
                    return false;
                }
            }

            auto defaults = MakeDefaultConfig();
            ValueCollector defaultValues;
            Describe(defaults, defaultValues);
            FileUpdater updater{ file, defaultValues.values };
            Describe(a_config, updater);

            std::string output;
            return file.Save(output, false) >= 0 && WriteFile(a_path, output);
        }

        std::atomic<std::shared_ptr<const PluginConfig>> s_config{ std::make_shared<const PluginConfig>(
            MakeDefaultConfig()) };
        std::atomic_uint64_t s_revision{ 0 };

        // Serializes file access and Apply, so the live configuration and the file
        // always change in the same order.
        std::mutex s_fileLock;
        std::filesystem::path s_path;
    }

    void Load()
    {
        const std::scoped_lock lock{ s_fileLock };
        auto config = MakeDefaultConfig();
        s_path = GetPluginDirectory() / kFileName;
        const auto& path = s_path;

        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
        {
            // Writing could truncate a file that exists but cannot be inspected.
            logs::warn("Config could not access {}: {}; using defaults", ToUtf8(path), error.message());
        }
        else if (!exists)
        {
            WriteDefaultFile(path);
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

        s_config.store(std::make_shared<const PluginConfig>(std::move(config)), std::memory_order_release);
    }

    std::shared_ptr<const PluginConfig> Get()
    {
        return s_config.load(std::memory_order_acquire);
    }

    std::uint64_t GetRevision()
    {
        return s_revision.load(std::memory_order_acquire);
    }

    bool Apply(PluginConfig a_config)
    {
        const std::scoped_lock lock{ s_fileLock };
        auto config = std::make_shared<const PluginConfig>(std::move(a_config));
        s_config.store(config, std::memory_order_release);
        const auto revision = s_revision.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (s_path.empty() || !SaveFile(s_path, *config))
        {
            logs::warn("Config applied revision {} but could not save {}; changes last until restart", revision,
                ToUtf8(s_path));
            return false;
        }
        logs::info("Config applied revision {} and saved {}", revision, ToUtf8(s_path));
        return true;
    }
}
