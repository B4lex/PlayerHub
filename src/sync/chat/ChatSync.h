#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatRoster.h"

#include <cstddef>
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
        // Where in sender the player's name starts.
        std::size_t nameOffset{ 0 };
        std::string text;
        Multiplayer::PlayerIdentity player;
        // Of players online when the line arrived.
        std::vector<ChatRoster::Mention> mentions;
        bool outgoing{ false };
    };

    struct ChatTarget
    {
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kNearby };
        Multiplayer::PlayerIdentity player;

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
