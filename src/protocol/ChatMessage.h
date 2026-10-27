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

    enum class ChatRejection : std::uint8_t
    {
        kNone,
        kInvalid,
        kRateLimited,
        kNotInWorld,
        kUnknownRecipient
    };

    [[nodiscard]] constexpr const char* GetChatRejectionName(ChatRejection a_rejection) noexcept
    {
        switch (a_rejection)
        {
            case ChatRejection::kNone:
                return "none";
            case ChatRejection::kInvalid:
                return "invalid";
            case ChatRejection::kRateLimited:
                return "rate limited";
            case ChatRejection::kNotInWorld:
                return "not in world";
            case ChatRejection::kUnknownRecipient:
                return "unknown recipient";
        }
        return "unknown";
    }

    struct PlayerIdentity
    {
        PeerSlot playerID{ 0 };
        std::string name;
        SessionID session{ 0 };
    };

    struct ChatMessage
    {
        ChatChannel channel{ ChatChannel::kGlobal };
        PlayerIdentity sender;
        PlayerIdentity recipient;
        std::string text;
        bool outgoing{ false };
    };

    inline constexpr std::size_t kMaximumChatTextCharacters = 512;
    // UTF-8 takes at most 4 bytes for a code point.
    inline constexpr std::size_t kMaximumChatTextBytes = kMaximumChatTextCharacters * 4;

    [[nodiscard]] inline std::size_t CountChatCharacters(std::string_view a_text)
    {
        std::size_t count = 0;
        for (const auto character : a_text)
        {
            count += (static_cast<unsigned char>(character) & 0xC0) != 0x80 ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] inline bool IsValidChatText(std::string_view a_text)
    {
        if (a_text.empty() || a_text.size() > kMaximumChatTextBytes || a_text.front() == ' ' || a_text.back() == ' '
            || CountChatCharacters(a_text) > kMaximumChatTextCharacters)
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
        std::vector<PlayerIdentity> players;
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
