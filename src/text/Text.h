#pragma once

#include "text/locale/TextId.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Text
{
    // An index into GetLanguages(), where English is first.
    enum class Language : std::uint8_t
    {
        kEnglish
    };

    struct LanguageInfo
    {
        // A translation's file name without ".po", or kEnglishCode.
        std::string code;
        // Its name in itself, or its code if it does not translate LanguageName.
        std::string name;
    };

    inline constexpr std::string_view kEnglishCode = "en";

    [[nodiscard]] bool IsLanguageCode(std::string_view a_code);

    // Loads the translations, reads the game's language, and selects it. Call on the main
    // thread once the game has read its INI files and Config::Load has run; before that,
    // text is English.
    void Initialize();

    // English, then the translations Initialize loaded, by code. Call on the main thread.
    [[nodiscard]] std::span<const LanguageInfo> GetLanguages();

    // The language whose code matches a_code in any case. Call on the main thread.
    [[nodiscard]] std::optional<Language> FindLanguage(std::string_view a_code);

    // FindLanguage(a_code), or the game's for an empty or unknown code. Call on the main
    // thread after Initialize.
    void Select(std::string_view a_code);

    [[nodiscard]] Language GetLanguage();

    // English before Initialize, or for a game language no translation names.
    [[nodiscard]] Language GetGameLanguage();

    // The view is NUL-terminated.
    [[nodiscard]] std::string_view Get(Id a_id);

    // Whatever the game's language.
    [[nodiscard]] std::string_view GetEnglish(Id a_id);

    [[nodiscard]] inline const char* GetCString(Id a_id)
    {
        return Get(a_id).data();
    }

    // The form for a_count of an Id whose English text lists plural forms.
    [[nodiscard]] std::string_view GetPlural(Id a_id, std::uint64_t a_count);

    template <class... Args>
    [[nodiscard]] std::string Format(Id a_id, const Args&... a_args)
    {
        return std::vformat(Get(a_id), std::make_format_args(a_args...));
    }

    template <class... Args>
    void FormatTo(std::string& a_out, Id a_id, const Args&... a_args)
    {
        a_out.clear();
        std::vformat_to(std::back_inserter(a_out), Get(a_id), std::make_format_args(a_args...));
    }

    // The count is argument 0, before a_args.
    template <class... Args>
    [[nodiscard]] std::string FormatPlural(Id a_id, std::uint64_t a_count, const Args&... a_args)
    {
        return std::vformat(GetPlural(a_id, a_count), std::make_format_args(a_count, a_args...));
    }

    struct Piece
    {
        std::string_view text;
        // The replacement field's argument index, or -1 for literal text.
        int argument{ -1 };
    };

    // Splits a validated format into its literal text and replacement fields, in order.
    // Returns the number of pieces, at most a_pieces.size().
    std::size_t Split(std::string_view a_format, std::span<Piece> a_pieces);
}
