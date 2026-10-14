#include "text/Utf8Case.h"

namespace Utf8Case
{
    namespace
    {
        // Letters and capitals of these blocks are all two-byte UTF-8 sequences.
        char32_t ToUpper(char32_t a_char)
        {
            const bool odd = (a_char & 1U) != 0;
            if ((a_char >= 0xE0 && a_char <= 0xFE && a_char != 0xF7) || (a_char >= 0x430 && a_char <= 0x44F))
            {
                return a_char - 0x20;
            }
            if (a_char >= 0x450 && a_char <= 0x45F)
            {
                return a_char - 0x50;
            }
            if (((a_char >= 0x100 && a_char <= 0x137) || (a_char >= 0x14A && a_char <= 0x177)
                    || (a_char >= 0x460 && a_char <= 0x481) || (a_char >= 0x48A && a_char <= 0x4BF))
                && odd)
            {
                return a_char - 1;
            }
            if (((a_char >= 0x139 && a_char <= 0x148) || (a_char >= 0x179 && a_char <= 0x17E)) && !odd)
            {
                return a_char - 1;
            }
            return a_char;
        }
    }

    void AppendUpper(std::string& a_out, std::string_view a_text)
    {
        for (std::size_t i = 0; i < a_text.size();)
        {
            const auto lead = static_cast<unsigned char>(a_text[i]);
            if (lead >= 'a' && lead <= 'z')
            {
                a_out += static_cast<char>(lead - ('a' - 'A'));
                ++i;
                continue;
            }
            const auto trail = i + 1 < a_text.size() ? static_cast<unsigned char>(a_text[i + 1]) : 0U;
            if ((lead & 0xE0U) != 0xC0U || (trail & 0xC0U) != 0x80U)
            {
                a_out += a_text[i];
                ++i;
                continue;
            }
            const auto upper = ToUpper(((lead & 0x1FU) << 6) | (trail & 0x3FU));
            a_out += static_cast<char>(0xC0U | (upper >> 6));
            a_out += static_cast<char>(0x80U | (upper & 0x3FU));
            i += 2;
        }
    }
}
