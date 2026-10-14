#pragma once

#include "protocol/ChatMessage.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ChatSync
{
    struct ChatLine
    {
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
        std::string sender;
        std::string text;
        Multiplayer::RosterEntry player;
        bool outgoing{ false };
    };

    struct ChatTarget
    {
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kNearby };
        Multiplayer::RosterEntry player;

        [[nodiscard]] bool operator==(const ChatTarget& a_other) const
        {
            return channel == a_other.channel
                && (channel != Multiplayer::ChatChannel::kWhisper || player.playerID == a_other.player.playerID
                    || player.name == a_other.player.name);
        }
    };

    std::expected<void, std::string> Send(const ChatTarget& a_target, std::string_view a_text);

    void DrainLines(std::vector<ChatLine>& a_lines);

    // The server's rejections of messages already sent.
    void DrainRejections(std::vector<std::string>& a_rejections);
}
