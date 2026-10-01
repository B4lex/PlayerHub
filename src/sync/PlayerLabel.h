#pragma once

#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace PlayerLabel
{
    [[nodiscard]] inline std::string Format(std::string_view a_name, std::uint16_t a_level)
    {
        std::string label{ a_name.empty() ? std::string_view{ "Unknown" } : a_name };
        if (a_level > 0)
        {
            // U+2022 BULLET as UTF-8, independent of the compiler's execution charset.
            std::format_to(std::back_inserter(label), " \xE2\x80\xA2 {} lvl", a_level);
        }
        return label;
    }
}
