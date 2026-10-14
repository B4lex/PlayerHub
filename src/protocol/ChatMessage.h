#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/Session.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Multiplayer
{
    enum class ChatChannel : std::uint8_t
    {
        kGlobal,
        kNearby,
        kWhisper
    };

    struct ChatSpeaker
    {
        PeerSlot playerID{ 0 };
        std::string name;
        std::uint16_t level{ 0 };
        SessionID session{ 0 };
    };

    struct ChatMessage
    {
        ChatChannel channel{ ChatChannel::kGlobal };
        ChatSpeaker sender;
        ChatSpeaker recipient;
        std::string text;
        bool outgoing{ false };
    };

    struct RosterEntry
    {
        PeerSlot playerID{ 0 };
        std::string name;
        SessionID session{ 0 };
    };

    inline constexpr std::size_t kMaximumChatTextBytes = 512;

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

    struct Roster
    {
        PeerSlot localPlayerID{ 0 };
        std::vector<RosterEntry> players;
    };

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
