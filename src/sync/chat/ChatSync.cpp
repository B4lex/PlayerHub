#include "sync/chat/ChatSync.h"
#include "network/ServerClient.h"
#include "sync/PlayerLabel.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <format>
#include <mutex>
#include <utility>

namespace ChatSync
{
    namespace
    {
        std::mutex s_rejectionsLock;
        std::vector<std::string> s_rejections;

        std::string DescribeRejection(std::string_view a_error)
        {
            if (a_error == "rate limited")
            {
                return "You are sending messages too quickly";
            }
            if (a_error == "not in world")
            {
                return "You are not in the world";
            }
            if (a_error == "unknown recipient")
            {
                return "That player is no longer online";
            }
            return "The server did not accept your message";
        }

        std::string FormatSender(const Multiplayer::ChatMessage& a_message)
        {
            const auto format = [](const Multiplayer::ChatSpeaker& a_speaker) {
                return PlayerLabel::Format(a_speaker.name);
            };
            if (a_message.channel != Multiplayer::ChatChannel::kWhisper)
            {
                return format(a_message.sender);
            }
            return a_message.outgoing ? std::format("To {}", format(a_message.recipient))
                                      : std::format("From {}", format(a_message.sender));
        }
    }

    std::expected<void, std::string> Send(const ChatTarget& a_target, std::string_view a_text)
    {
        const auto text = Multiplayer::TrimChatText(a_text);
        if (text.empty())
        {
            return {};
        }
        if (!Multiplayer::IsValidChatText(text))
        {
            return std::unexpected{ "Your message contains characters that cannot be sent" };
        }
        Multiplayer::PeerSlot recipient = 0;
        if (a_target.channel == Multiplayer::ChatChannel::kWhisper)
        {
            if (!Network::IsConnected())
            {
                return std::unexpected{ "You are not connected to the server" };
            }
            const auto player = ChatRoster::Resolve(a_target.player);
            if (!player)
            {
                return std::unexpected{ std::format("{} is offline", a_target.player.name) };
            }
            recipient = player->playerID;
        }
        if (!Network::PublishChatMessage(a_target.channel, recipient, text, [](std::string_view a_error) {
                auto rejection = DescribeRejection(a_error);
                logs::debug("Chat rejected: {}", rejection);
                const std::scoped_lock lock{ s_rejectionsLock };
                s_rejections.push_back(std::move(rejection));
            }))
        {
            return std::unexpected{ "You are not connected to the server" };
        }
        logs::debug("Chat sent: channel={} recipient={} bytes={}", static_cast<int>(a_target.channel), recipient,
            text.size());
        return {};
    }

    void DrainLines(std::vector<ChatLine>& a_lines)
    {
        for (auto& message : Network::DrainChatMessages())
        {
            const bool whisper = message.channel == Multiplayer::ChatChannel::kWhisper;
            const auto& other = whisper && message.outgoing ? message.recipient : message.sender;
            ChatLine line{ .channel = message.channel,
                .sender = FormatSender(message),
                .text = std::move(message.text),
                .player = { .playerID = other.playerID, .name = other.name, .session = other.session },
                .outgoing = message.outgoing };
            // A note to oneself is not a conversation, and a muted player's whispers are
            // hidden, so neither makes a whisper partner.
            if (whisper && message.sender.playerID != message.recipient.playerID
                && (message.outgoing || !PlayerFilters::IsMuted(message.sender.session)))
            {
                ChatRoster::NoteWhisper(line.player);
            }
            a_lines.push_back(std::move(line));
        }
    }

    void DrainRejections(std::vector<std::string>& a_rejections)
    {
        const std::scoped_lock lock{ s_rejectionsLock };
        for (auto& rejection : s_rejections)
        {
            a_rejections.push_back(std::move(rejection));
        }
        s_rejections.clear();
    }
}
