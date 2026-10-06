#pragma once

#include "protocol/ChatMessage.h"

#include <string>
#include <string_view>
#include <vector>

namespace ChatSync
{
    enum class LineKind
    {
        kPlayer,
        kNotice
    };

    struct ChatLine
    {
        LineKind kind{ LineKind::kPlayer };
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
        std::string sender;
        std::string text;
        Multiplayer::RosterEntry player;
        bool outgoing{ false };
    };

    struct ChatTarget
    {
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kLocal };
        Multiplayer::RosterEntry player;

        [[nodiscard]] bool operator==(const ChatTarget& a_other) const
        {
            return channel == a_other.channel
                && (channel != Multiplayer::ChatChannel::kWhisper || player.playerID == a_other.player.playerID
                    || player.name == a_other.player.name);
        }
    };

    bool Send(const ChatTarget& a_target, std::string_view a_text);

    void AddNotice(std::string a_text);

    void DrainLines(std::vector<ChatLine>& a_lines);
}
