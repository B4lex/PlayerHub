#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Multiplayer
{
    struct ChatMessage
    {
        std::string playerID;
        // The sender's character name, empty until its appearance is known.
        std::string senderName;
        std::string text;
    };

    // UTF-8 bytes, matching the server's MAXIMUM_CHAT_TEXT_BYTES.
    inline constexpr std::size_t kMaximumChatTextBytes = 512;

    // Trimmed, non-empty text without control characters. Shared by the sender
    // and the network parser so both sides enforce the same bounds as the server.
    [[nodiscard]] inline bool IsValidChatText(std::string_view a_text)
    {
        if (a_text.empty() || a_text.size() > kMaximumChatTextBytes || a_text.front() == ' '
            || a_text.back() == ' ')
        {
            return false;
        }
        for (const auto character : a_text)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte == 0x7F)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::string_view TrimChatText(std::string_view a_text)
    {
        const auto first = a_text.find_first_not_of(' ');
        if (first == std::string_view::npos)
        {
            return {};
        }
        return a_text.substr(first, a_text.find_last_not_of(' ') - first + 1);
    }
}
