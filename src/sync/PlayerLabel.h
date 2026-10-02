#pragma once

#include <string>
#include <string_view>

namespace PlayerLabel
{
    [[nodiscard]] inline std::string Format(std::string_view a_name)
    {
        return std::string{ a_name.empty() ? std::string_view{ "Unknown" } : a_name };
    }
}
