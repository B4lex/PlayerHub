#include "text/Text.h"
#include "text/AsciiCase.h"
#include "text/TextTables.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>

namespace Text
{
    namespace
    {
        constexpr std::size_t kIdCount = static_cast<std::size_t>(Id::kCount);
        constexpr std::size_t kMaximumPluralForms = 3;
        constexpr std::size_t kMaximumFields = 8;
        constexpr char kPluralSeparator = '|';

        struct LanguageInfo
        {
            Language language;
            // sLanguage:General, which also names the game's string tables.
            std::string_view gameName;
            std::span<const Tables::Entry> (*entries)();
            std::size_t pluralForms;
        };

        constexpr std::array kLanguages{
            LanguageInfo{ Language::kEnglish, "ENGLISH", &Tables::GetEnglish, 2 },
            LanguageInfo{ Language::kFrench, "FRENCH", &Tables::GetFrench, 2 },
            LanguageInfo{ Language::kGerman, "GERMAN", &Tables::GetGerman, 2 },
            LanguageInfo{ Language::kItalian, "ITALIAN", &Tables::GetItalian, 2 },
            LanguageInfo{ Language::kSpanish, "SPANISH", &Tables::GetSpanish, 2 },
            LanguageInfo{ Language::kPolish, "POLISH", &Tables::GetPolish, 3 },
            LanguageInfo{ Language::kRussian, "RUSSIAN", &Tables::GetRussian, 3 },
            LanguageInfo{ Language::kJapanese, "JAPANESE", &Tables::GetJapanese, 1 },
            LanguageInfo{ Language::kChinese, "CHINESE", &Tables::GetChinese, 1 },
        };

        const LanguageInfo& GetInfo(Language a_language)
        {
            return kLanguages[static_cast<std::size_t>(a_language)];
        }

        using Forms = std::array<std::string_view, kMaximumPluralForms>;

        struct Table
        {
            std::array<std::string_view, kIdCount> text{};
            std::array<Forms, kIdCount> plurals{};
            // The language whose plural rule picks among an Id's forms; English where a
            // translation's forms were rejected.
            std::array<Language, kIdCount> pluralRules{};
            Language language{ Language::kEnglish };
        };

        std::size_t GetPluralIndex(Language a_language, std::uint64_t a_count)
        {
            const auto lastDigit = a_count % 10;
            const auto lastTwoDigits = a_count % 100;
            const bool few = lastDigit >= 2 && lastDigit <= 4 && (lastTwoDigits < 12 || lastTwoDigits > 14);
            switch (a_language)
            {
                case Language::kFrench:
                    return a_count <= 1 ? 0 : 1;
                case Language::kPolish:
                    return a_count == 1 ? 0 : few ? 1 : 2;
                case Language::kRussian:
                    return lastDigit == 1 && lastTwoDigits != 11 ? 0 : few ? 1 : 2;
                case Language::kJapanese:
                case Language::kChinese:
                    return 0;
                default:
                    return a_count == 1 ? 0 : 1;
            }
        }

        // Splits at the separator; returns the count, or 0 if there are more than the maximum.
        std::size_t SplitForms(std::string_view a_text, Forms& a_forms)
        {
            std::size_t count = 0;
            while (true)
            {
                if (count == a_forms.size())
                {
                    return 0;
                }
                const auto separator = a_text.find(kPluralSeparator);
                a_forms[count++] = a_text.substr(0, separator);
                if (separator == std::string_view::npos)
                {
                    return count;
                }
                a_text.remove_prefix(separator + 1);
            }
        }

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

        Table BuildEnglish()
        {
            Table table;
            // A literal, so even a missing text is NUL-terminated.
            table.text.fill("");
            for (const auto& entry : Tables::GetEnglish())
            {
                const auto index = static_cast<std::size_t>(entry.id);
                if (index >= kIdCount)
                {
                    continue;
                }
                table.text[index] = entry.text;
                if (entry.text.find(kPluralSeparator) != std::string_view::npos)
                {
                    SplitForms(entry.text, table.plurals[index]);
                }
            }
            return table;
        }

        const Table s_english = BuildEnglish();
        Table s_translated;
        std::atomic<const Table*> s_active{ &s_english };
        std::once_flag s_initialized;

        bool IsPlural(const Table& a_english, std::size_t a_index)
        {
            return !a_english.plurals[a_index][0].empty();
        }

        std::optional<Fields> GetEnglishFields(const Table& a_english, std::size_t a_index)
        {
            if (!IsPlural(a_english, a_index))
            {
                return ParseFields(a_english.text[a_index]);
            }
            Fields all;
            for (const auto form : a_english.plurals[a_index])
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
                else if (!GetEnglishFields(s_english, index))
                {
                    logs::error("Localization: English text for ID {} is not a valid format", index);
                }
            }
            if (missing > 0)
            {
                logs::error("Localization: English has no text for {} IDs", missing);
            }
        }

        bool Translate(Table& a_table, const LanguageInfo& a_info, std::size_t a_index, std::string_view a_text)
        {
            const auto english = GetEnglishFields(s_english, a_index);
            if (!english)
            {
                return false;
            }
            if (!IsPlural(s_english, a_index))
            {
                if (a_text.find(kPluralSeparator) != std::string_view::npos || !MatchesEnglish(*english, a_text, true))
                {
                    return false;
                }
                a_table.text[a_index] = a_text;
                return true;
            }
            Forms forms{};
            if (SplitForms(a_text, forms) != a_info.pluralForms)
            {
                return false;
            }
            for (std::size_t form = 0; form < a_info.pluralForms; ++form)
            {
                if (!MatchesEnglish(*english, forms[form], false))
                {
                    return false;
                }
            }
            a_table.text[a_index] = a_text;
            a_table.plurals[a_index] = forms;
            a_table.pluralRules[a_index] = a_info.language;
            return true;
        }

        void Load(const LanguageInfo& a_info)
        {
            auto& table = s_translated;
            table = s_english;
            table.language = a_info.language;
            std::array<bool, kIdCount> translated{};
            std::size_t rejected = 0;
            for (const auto& entry : a_info.entries())
            {
                const auto index = static_cast<std::size_t>(entry.id);
                if (index == 0 || index >= kIdCount || translated[index])
                {
                    logs::warn("Localization: {} lists ID {} twice or out of range", a_info.gameName, index);
                    continue;
                }
                if (!Translate(table, a_info, index, entry.text))
                {
                    ++rejected;
                    logs::warn(
                        "Localization: {} text for ID {} does not match the English fields or plural forms; "
                        "using English",
                        a_info.gameName, index);
                    continue;
                }
                translated[index] = true;
            }
            const auto missing = static_cast<std::size_t>(std::count(translated.begin() + 1, translated.end(), false));
            logs::info("Localization: {} with {} of {} texts translated ({} rejected)", a_info.gameName,
                kIdCount - 1 - missing, kIdCount - 1, rejected);
            s_active.store(&table, std::memory_order_release);
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

    void Initialize()
    {
        std::call_once(s_initialized, [] {
            CheckEnglish();
            const auto gameLanguage = ReadGameLanguage();
            const auto info = std::ranges::find_if(kLanguages,
                [&](const LanguageInfo& a_info) { return AsciiCase::Equals(a_info.gameName, gameLanguage); });
            if (info == kLanguages.end())
            {
                logs::info("Localization: game language '{}' has no translation; using English", gameLanguage);
                return;
            }
            if (info->language == Language::kEnglish)
            {
                logs::info("Localization: ENGLISH");
                return;
            }
            Load(*info);
        });
    }

    Language GetLanguage()
    {
        return s_active.load(std::memory_order_acquire)->language;
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
        const auto form = table.plurals[index][GetPluralIndex(table.pluralRules[index], a_count)];
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
