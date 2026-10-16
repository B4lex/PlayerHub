#include "text/Text.h"
#include "config/PluginConfig.h"
#include "config/PluginFiles.h"
#include "text/AsciiCase.h"
#include "text/Gettext.h"
#include "text/locale/English.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Text
{
    namespace
    {
        constexpr std::size_t kIdCount = static_cast<std::size_t>(Id::kCount);
        constexpr std::size_t kMaximumFields = 8;
        using Catalogs::kMaximumPluralForms;
        constexpr std::size_t kMaximumCodeLength = 32;
        // Language is a std::uint8_t index.
        constexpr std::size_t kMaximumLanguages = 255;
        constexpr std::uintmax_t kMaximumFileSize = 4 * 1024 * 1024;
        constexpr std::wstring_view kDirectory = L"PlayerHub/locale";
        constexpr std::string_view kExtension = ".po";
        constexpr std::string_view kEnglishGameName = "ENGLISH";
        // The sLanguage:General value, which also names the game's string tables, that
        // selects a translation while [UI] Language is empty.
        constexpr std::string_view kGameLanguageHeader = "X-Skyrim-Language";

        using Forms = std::array<std::string_view, kMaximumPluralForms>;

        struct Table
        {
            std::array<std::string_view, kIdCount> text{};
            // Empty for an Id without plural forms.
            std::array<Forms, kIdCount> plurals{};
            // The rule that picks among an Id's forms: its catalog's, or English's where a
            // translation's forms were rejected.
            std::array<const Gettext::PluralRule*, kIdCount> pluralRules{};
            Language language{ Language::kEnglish };
        };

        struct Field
        {
            int index{ 0 };
            // From the colon on, or empty.
            std::string_view spec;
        };

        struct Fields
        {
            std::array<Field, kMaximumFields> items{};
            std::size_t count{ 0 };

            [[nodiscard]] bool Has(const Field& a_field) const
            {
                return std::any_of(items.begin(), items.begin() + count,
                    [&](const Field& a_item) { return a_item.index == a_field.index && a_item.spec == a_field.spec; });
            }

            [[nodiscard]] bool HasIndex(int a_index) const
            {
                return std::any_of(
                    items.begin(), items.begin() + count, [&](const Field& a_item) { return a_item.index == a_index; });
            }
        };

        // Calls a_literal(text) and a_field(field, text) in order. Nested fields in a format
        // spec are not supported. Returns false where std::format would reject the format.
        template <class Literal, class FieldVisitor>
        bool Scan(std::string_view a_format, Literal&& a_literal, FieldVisitor&& a_field)
        {
            int next = 0;
            bool automatic = false;
            bool manual = false;
            std::size_t start = 0;
            for (std::size_t i = 0; i < a_format.size(); ++i)
            {
                const char character = a_format[i];
                if (character != '{' && character != '}')
                {
                    continue;
                }
                if (i + 1 < a_format.size() && a_format[i + 1] == character)
                {
                    // An escaped brace: the text up to and including the first one.
                    a_literal(a_format.substr(start, i + 1 - start));
                    start = i + 2;
                    ++i;
                    continue;
                }
                if (character == '}')
                {
                    return false;
                }
                const auto close = a_format.find('}', i + 1);
                if (close == std::string_view::npos)
                {
                    return false;
                }
                const auto content = a_format.substr(i + 1, close - i - 1);
                if (content.find('{') != std::string_view::npos)
                {
                    return false;
                }
                const auto colon = content.find(':');
                const auto indexText = content.substr(0, colon);
                Field field{ .spec = colon == std::string_view::npos ? std::string_view{} : content.substr(colon) };
                if (indexText.empty())
                {
                    automatic = true;
                    field.index = next++;
                }
                else
                {
                    manual = true;
                    for (const auto digit : indexText)
                    {
                        if (digit < '0' || digit > '9' || field.index > 99)
                        {
                            return false;
                        }
                        field.index = field.index * 10 + (digit - '0');
                    }
                }
                if (automatic && manual)
                {
                    return false;
                }
                a_literal(a_format.substr(start, i - start));
                a_field(field, a_format.substr(i, close + 1 - i));
                start = close + 1;
                i = close;
            }
            a_literal(a_format.substr(start));
            return true;
        }

        std::optional<Fields> ParseFields(std::string_view a_format)
        {
            Fields fields;
            bool overflow = false;
            const bool valid = Scan(
                a_format, [](std::string_view) {},
                [&](const Field& a_field, std::string_view) {
                    if (fields.count == fields.items.size())
                    {
                        overflow = true;
                        return;
                    }
                    fields.items[fields.count++] = a_field;
                });
            return valid && !overflow ? std::optional{ fields } : std::nullopt;
        }

        // A translation may use only fields the English text has, so formatting cannot fail;
        // a text that is not a plural form must also use every argument.
        bool MatchesEnglish(const Fields& a_english, std::string_view a_translation, bool a_usesAll)
        {
            const auto fields = ParseFields(a_translation);
            if (!fields)
            {
                return false;
            }
            for (std::size_t i = 0; i < fields->count; ++i)
            {
                if (!a_english.Has(fields->items[i]))
                {
                    return false;
                }
            }
            if (a_usesAll)
            {
                for (std::size_t i = 0; i < a_english.count; ++i)
                {
                    if (!fields->HasIndex(a_english.items[i].index))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        const Gettext::PluralRule s_englishRule = [] {
            auto rule = Gettext::PluralRule::Parse(Catalogs::GetEnglish().pluralForms, kMaximumPluralForms);
            return rule ? std::move(*rule) : Gettext::PluralRule{};
        }();

        Table BuildEnglish()
        {
            Table table;
            // A literal, so even a missing text is NUL-terminated.
            table.text.fill("");
            table.pluralRules.fill(&s_englishRule);
            for (const auto& entry : Catalogs::GetEnglish().entries)
            {
                const auto index = static_cast<std::size_t>(entry.id);
                if (index >= kIdCount)
                {
                    continue;
                }
                table.text[index] = entry.text;
                if (!entry.plural.empty())
                {
                    table.plurals[index][0] = entry.text;
                    table.plurals[index][1] = entry.plural;
                }
            }
            return table;
        }

        const Table s_english = BuildEnglish();

        struct Translation
        {
            Gettext::PluralRule rule;
            // The texts the table views; a deque never moves them.
            std::deque<std::string> texts;
            std::string gameName;
            Table table;
        };

        // Built by Initialize and kept for the session: another thread may still be reading
        // a table after another one is selected. Index 0, English, has no Translation.
        std::vector<LanguageInfo> s_languages{ LanguageInfo{
            std::string{ kEnglishCode }, std::string{ s_english.text[static_cast<std::size_t>(Id::kLanguageName)] } } };
        std::vector<std::unique_ptr<const Translation>> s_translations(1);
        std::atomic<const Table*> s_active{ &s_english };
        std::atomic<Language> s_gameLanguage{ Language::kEnglish };
        std::once_flag s_initialized;
        // The last code Select warned of, so a config change that keeps it does not repeat the warning.
        std::string s_unknownCode;

        bool IsPlural(std::size_t a_index)
        {
            return !s_english.plurals[a_index][0].empty();
        }

        std::optional<Fields> GetEnglishFields(std::size_t a_index)
        {
            if (!IsPlural(a_index))
            {
                return ParseFields(s_english.text[a_index]);
            }
            Fields all;
            for (const auto form : s_english.plurals[a_index])
            {
                const auto fields = ParseFields(form);
                if (!fields)
                {
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < fields->count; ++i)
                {
                    if (!all.Has(fields->items[i]) && all.count < all.items.size())
                    {
                        all.items[all.count++] = fields->items[i];
                    }
                }
            }
            return all;
        }

        void CheckEnglish()
        {
            std::size_t missing = 0;
            for (std::size_t index = 1; index < kIdCount; ++index)
            {
                if (s_english.text[index].empty())
                {
                    ++missing;
                    logs::error("Localization: English has no text for ID {}", index);
                }
                else if (!GetEnglishFields(index))
                {
                    logs::error("Localization: English text for ID {} is not a valid format", index);
                }
            }
            if (missing > 0)
            {
                logs::error("Localization: English has no text for {} IDs", missing);
            }
        }

        enum class Outcome
        {
            kTranslated,
            kUntranslated,
            kFuzzy,
            kRejected
        };

        // Gettext uses neither a fuzzy entry nor one with an empty msgstr.
        Outcome Translate(Translation& a_translation, std::size_t a_index, const Gettext::Message& a_message,
            std::string_view a_file)
        {
            const auto reject = [&](std::string_view a_reason) {
                logs::warn("Localization: {}:{}: {} {}; using English", a_file, a_message.line, a_message.context,
                    a_reason);
                return Outcome::kRejected;
            };
            if (std::ranges::all_of(a_message.forms, [](const std::string& a_form) { return a_form.empty(); }))
            {
                return Outcome::kUntranslated;
            }
            if (a_message.fuzzy)
            {
                return Outcome::kFuzzy;
            }
            const bool plural = IsPlural(a_index);
            if (a_message.id != s_english.text[a_index] || a_message.hasPlural != plural
                || (plural && a_message.plural != s_english.plurals[a_index][1]))
            {
                return reject("translates another English text");
            }
            const auto english = GetEnglishFields(a_index);
            if (!english)
            {
                return reject("has no valid English text");
            }
            const auto count = plural ? a_translation.rule.GetFormCount() : 1;
            if (a_message.forms.size() != count
                || std::ranges::any_of(a_message.forms, [](const std::string& a_form) { return a_form.empty(); }))
            {
                return reject(std::format("needs {} translated form(s)", count));
            }
            for (const auto& form : a_message.forms)
            {
                if (!MatchesEnglish(*english, form, !plural))
                {
                    return reject("does not match the English text's fields");
                }
            }

            auto& table = a_translation.table;
            for (std::size_t form = 0; form < count; ++form)
            {
                const std::string_view text = a_translation.texts.emplace_back(a_message.forms[form]);
                if (plural)
                {
                    table.plurals[a_index][form] = text;
                }
                if (form == 0)
                {
                    table.text[a_index] = text;
                }
            }
            if (plural)
            {
                table.pluralRules[a_index] = &a_translation.rule;
            }
            return Outcome::kTranslated;
        }

        using KeyIndex = std::unordered_map<std::string_view, std::size_t>;

        std::unique_ptr<const Translation> Load(
            const Gettext::File& a_file, Gettext::PluralRule a_rule, Language a_language, std::string_view a_name,
            const KeyIndex& a_keys)
        {
            auto translation = std::make_unique<Translation>();
            translation->rule = std::move(a_rule);
            translation->table = s_english;
            translation->gameName = a_file.GetHeader(kGameLanguageHeader);
            translation->table.language = a_language;
            std::array<bool, kIdCount> seen{};
            std::array<std::size_t, 4> outcomes{};
            for (const auto& message : a_file.messages)
            {
                const auto key = a_keys.find(message.context);
                if (key == a_keys.end())
                {
                    logs::warn("Localization: {}:{}: {} is not a text this version has", a_name, message.line,
                        message.context);
                    continue;
                }
                if (seen[key->second])
                {
                    logs::warn("Localization: {}:{}: {} is listed twice", a_name, message.line, message.context);
                    continue;
                }
                seen[key->second] = true;
                ++outcomes[static_cast<std::size_t>(Translate(*translation, key->second, message, a_name))];
            }
            const auto translated = outcomes[static_cast<std::size_t>(Outcome::kTranslated)];
            logs::info("Localization: {} with {} of {} texts translated ({} fuzzy, {} rejected){}", a_name, translated,
                kIdCount - 1, outcomes[static_cast<std::size_t>(Outcome::kFuzzy)],
                outcomes[static_cast<std::size_t>(Outcome::kRejected)],
                translation->gameName.empty() ? std::string{} : std::format(" for {}", translation->gameName));
            return translation;
        }

        std::vector<std::filesystem::path> FindFiles(const std::filesystem::path& a_directory)
        {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::directory_iterator entry{ a_directory, error }, end; !error && entry != end;
                 entry.increment(error))
            {
                if (entry->is_regular_file(error)
                    && AsciiCase::Equals(PluginFiles::ToUtf8(entry->path().extension()), kExtension))
                {
                    files.push_back(entry->path());
                }
            }
            if (error)
            {
                logs::warn("Localization: could not list {}: {}", PluginFiles::ToUtf8(a_directory), error.message());
            }
            std::ranges::sort(files);
            return files;
        }

        void LoadTranslations()
        {
            const auto base = Config::GetDirectory();
            if (base.empty())
            {
                return;
            }
            const auto directory = base / kDirectory;
            KeyIndex keys;
            for (const auto& entry : Catalogs::GetEnglish().entries)
            {
                keys.emplace(entry.key, static_cast<std::size_t>(entry.id));
            }
            for (const auto& path : FindFiles(directory))
            {
                const auto name = PluginFiles::ToUtf8(path.filename());
                const auto code = PluginFiles::ToUtf8(path.stem());
                const auto skip = [&](std::string_view a_reason) {
                    logs::warn("Localization: skipping {}: {}", name, a_reason);
                };
                if (!IsLanguageCode(code) || AsciiCase::Equals(code, kEnglishCode))
                {
                    skip("the file name is not a language code other than en");
                    continue;
                }
                if (FindLanguage(code))
                {
                    skip("another file has the same code");
                    continue;
                }
                if (s_languages.size() == kMaximumLanguages)
                {
                    skip(std::format("more than {} languages", kMaximumLanguages - 1));
                    continue;
                }
                std::error_code error;
                const auto size = std::filesystem::file_size(path, error);
                if (error || size > kMaximumFileSize)
                {
                    skip(error ? error.message() : std::string{ "the file is too large" });
                    continue;
                }
                const auto text = PluginFiles::ReadFile(path);
                if (!text)
                {
                    skip("could not read the file");
                    continue;
                }
                auto file = Gettext::Parse(*text);
                if (!file)
                {
                    skip(file.error());
                    continue;
                }
                auto rule = Gettext::PluralRule::Parse(file->GetHeader("Plural-Forms"), kMaximumPluralForms);
                if (!rule)
                {
                    skip(std::format("Plural-Forms: {}", rule.error()));
                    continue;
                }
                const auto language = static_cast<Language>(s_languages.size());
                auto translation = Load(*file, std::move(*rule), language, name, keys);
                const auto ownName = translation->table.text[static_cast<std::size_t>(Id::kLanguageName)];
                s_languages.push_back(LanguageInfo{ code,
                    std::string{ ownName == s_english.text[static_cast<std::size_t>(Id::kLanguageName)] ? code : ownName } });
                s_translations.push_back(std::move(translation));
            }
        }

        const Table& GetTable(Language a_language)
        {
            const auto index = static_cast<std::size_t>(a_language);
            return index == 0 || index >= s_translations.size() ? s_english : s_translations[index]->table;
        }

        std::string_view ReadGameLanguage()
        {
            auto* collection = RE::INISettingCollection::GetSingleton();
            auto* setting = collection ? collection->GetSetting("sLanguage:General") : nullptr;
            const char* value
                = setting && setting->GetType() == RE::Setting::Type::kString ? setting->GetString() : nullptr;
            return value ? std::string_view{ value } : std::string_view{};
        }
    }

    bool IsLanguageCode(std::string_view a_code)
    {
        return !a_code.empty() && a_code.size() <= kMaximumCodeLength
            && AsciiCase::ToLower(a_code.front()) >= 'a' && AsciiCase::ToLower(a_code.front()) <= 'z'
            && std::ranges::all_of(a_code, [](char a_character) {
                   const auto lower = AsciiCase::ToLower(a_character);
                   return (lower >= 'a' && lower <= 'z') || (a_character >= '0' && a_character <= '9')
                       || a_character == '_' || a_character == '@' || a_character == '-';
               });
    }

    void Initialize()
    {
        std::call_once(s_initialized, [] {
            CheckEnglish();
            LoadTranslations();
            const auto gameName = ReadGameLanguage();
            auto language = Language::kEnglish;
            if (!AsciiCase::Equals(gameName, kEnglishGameName))
            {
                const auto translation = std::ranges::find_if(s_translations, [&](const auto& a_translation) {
                    return a_translation && AsciiCase::Equals(a_translation->gameName, gameName);
                });
                if (translation == s_translations.end())
                {
                    logs::info("Localization: no translation for the game's language '{}'; using English", gameName);
                    return;
                }
                language = (*translation)->table.language;
            }
            s_gameLanguage.store(language, std::memory_order_relaxed);
            s_active.store(&GetTable(language), std::memory_order_release);
            logs::info("Localization: the game's language {} is {}", gameName,
                s_languages[static_cast<std::size_t>(language)].code);
        });
    }

    std::span<const LanguageInfo> GetLanguages()
    {
        return s_languages;
    }

    std::optional<Language> FindLanguage(std::string_view a_code)
    {
        const auto found = std::ranges::find_if(
            s_languages, [&](const LanguageInfo& a_language) { return AsciiCase::Equals(a_language.code, a_code); });
        return found == s_languages.end() ? std::nullopt
                                          : std::optional{ static_cast<Language>(found - s_languages.begin()) };
    }

    void Select(std::string_view a_code)
    {
        auto language = GetGameLanguage();
        if (const auto found = FindLanguage(a_code))
        {
            language = *found;
        }
        else if (!a_code.empty() && a_code != s_unknownCode)
        {
            s_unknownCode = a_code;
            logs::warn("Localization: no {}{} in {}; using the game's language", a_code, kExtension,
                PluginFiles::ToUtf8(std::filesystem::path{ kDirectory }));
        }
        if (language == GetLanguage())
        {
            return;
        }
        s_active.store(&GetTable(language), std::memory_order_release);
        logs::info("Localization: switched to {}{}", s_languages[static_cast<std::size_t>(language)].code,
            a_code.empty() ? " (the game's)" : "");
    }

    Language GetLanguage()
    {
        return s_active.load(std::memory_order_acquire)->language;
    }

    Language GetGameLanguage()
    {
        return s_gameLanguage.load(std::memory_order_relaxed);
    }

    std::string_view Get(Id a_id)
    {
        const auto index = static_cast<std::size_t>(a_id);
        return index < kIdCount ? s_active.load(std::memory_order_acquire)->text[index] : std::string_view{ "" };
    }

    std::string_view GetEnglish(Id a_id)
    {
        const auto index = static_cast<std::size_t>(a_id);
        return index < kIdCount ? s_english.text[index] : std::string_view{ "" };
    }

    std::string_view GetPlural(Id a_id, std::uint64_t a_count)
    {
        const auto index = static_cast<std::size_t>(a_id);
        if (index >= kIdCount)
        {
            return {};
        }
        const auto& table = *s_active.load(std::memory_order_acquire);
        if (table.plurals[index][0].empty())
        {
            return table.text[index];
        }
        const auto form = table.plurals[index][(std::min)(table.pluralRules[index]->GetForm(a_count),
            kMaximumPluralForms - 1)];
        return form.empty() ? table.text[index] : form;
    }

    std::size_t Split(std::string_view a_format, std::span<Piece> a_pieces)
    {
        std::size_t count = 0;
        const auto add = [&](Piece a_piece) {
            if (!a_piece.text.empty() && count < a_pieces.size())
            {
                a_pieces[count++] = a_piece;
            }
        };
        Scan(
            a_format, [&](std::string_view a_text) { add(Piece{ .text = a_text }); },
            [&](const Field& a_field, std::string_view a_text) {
                add(Piece{ .text = a_text, .argument = a_field.index });
            });
        return count;
    }
}
