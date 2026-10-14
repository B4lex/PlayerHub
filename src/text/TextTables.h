#pragma once

#include "text/TextId.h"

#include <span>
#include <string_view>

namespace Text::Tables
{
    struct Entry
    {
        Id id;
        std::string_view text;
    };

    [[nodiscard]] std::span<const Entry> GetEnglish();
    [[nodiscard]] std::span<const Entry> GetFrench();
    [[nodiscard]] std::span<const Entry> GetGerman();
    [[nodiscard]] std::span<const Entry> GetItalian();
    [[nodiscard]] std::span<const Entry> GetSpanish();
    [[nodiscard]] std::span<const Entry> GetPolish();
    [[nodiscard]] std::span<const Entry> GetRussian();
    [[nodiscard]] std::span<const Entry> GetJapanese();
    [[nodiscard]] std::span<const Entry> GetChinese();
}
