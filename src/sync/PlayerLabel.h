#pragma once

#include "text/Text.h"

#include <string>
#include <string_view>

namespace PlayerLabel
{
    [[nodiscard]] inline std::string Format(std::string_view a_name)
    {
        return std::string{ a_name.empty() ? Text::Get(Text::Id::kUnknownPlayer) : a_name };
    }
}
