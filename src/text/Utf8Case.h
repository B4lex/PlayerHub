#pragma once

#include <string>
#include <string_view>

namespace Utf8Case
{
    // Capitalizes ASCII, Latin-1, Latin Extended-A, and Cyrillic letters; anything else is kept.
    void AppendUpper(std::string& a_out, std::string_view a_text);
}
