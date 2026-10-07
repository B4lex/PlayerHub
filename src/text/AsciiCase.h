#pragma once

#include <cstddef>
#include <string_view>

// Case folding of ASCII letters only, independent of the C locale, so UTF-8 text
// compares byte for byte outside A-Z.
namespace AsciiCase
{
    [[nodiscard]] constexpr char ToLower(char a_character) noexcept
    {
        return a_character >= 'A' && a_character <= 'Z' ? static_cast<char>(a_character - 'A' + 'a') : a_character;
    }

    [[nodiscard]] constexpr bool Equals(std::string_view a_left, std::string_view a_right) noexcept
    {
        if (a_left.size() != a_right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < a_left.size(); ++index)
        {
            if (ToLower(a_left[index]) != ToLower(a_right[index]))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr std::size_t Find(
        std::string_view a_text, std::string_view a_part, std::size_t a_start = 0) noexcept
    {
        for (auto start = a_start; start + a_part.size() <= a_text.size(); ++start)
        {
            if (Equals(a_text.substr(start, a_part.size()), a_part))
            {
                return start;
            }
        }
        return std::string_view::npos;
    }

    [[nodiscard]] constexpr bool Contains(std::string_view a_text, std::string_view a_part) noexcept
    {
        return Find(a_text, a_part) != std::string_view::npos;
    }

    [[nodiscard]] constexpr bool StartsWith(std::string_view a_text, std::string_view a_prefix) noexcept
    {
        return a_text.size() >= a_prefix.size() && Equals(a_text.substr(0, a_prefix.size()), a_prefix);
    }

    [[nodiscard]] constexpr bool EndsWith(std::string_view a_text, std::string_view a_suffix) noexcept
    {
        return a_text.size() >= a_suffix.size() && Equals(a_text.substr(a_text.size() - a_suffix.size()), a_suffix);
    }

    // Negative, zero, or positive, ordering bytes as unsigned.
    [[nodiscard]] constexpr int Compare(std::string_view a_left, std::string_view a_right) noexcept
    {
        const auto count = a_left.size() < a_right.size() ? a_left.size() : a_right.size();
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto left = static_cast<unsigned char>(ToLower(a_left[index]));
            const auto right = static_cast<unsigned char>(ToLower(a_right[index]));
            if (left != right)
            {
                return left < right ? -1 : 1;
            }
        }
        return a_left.size() == a_right.size() ? 0 : (a_left.size() < a_right.size() ? -1 : 1);
    }

    static_assert(Equals("bashExit", "BASHEXIT"));
    static_assert(!Equals("bash", "bashExit"));
    static_assert(Find("attackStartLeftHand", "lefthand") == 11);
    static_assert(Find("abc", "") == 0 && Find("", "") == 0 && Find("", "a") == std::string_view::npos);
    static_assert(EndsWith("attackStartH2HLeft", "h2hleft") && !EndsWith("Left", "H2HLeft"));
    static_assert(Compare("Skyrim.esm", "skyrim.ESM") == 0 && Compare("a", "B") < 0 && Compare("ab", "A") > 0);
}
