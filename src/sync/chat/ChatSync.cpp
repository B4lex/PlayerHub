#include "sync/chat/ChatSync.h"
#include "network/ServerClient.h"
#include "protocol/ChatMessage.h"
#include "sync/PlayerLabel.h"

#include <mutex>
#include <utility>

namespace ChatSync
{
    namespace
    {
        // Rejections are reported on the transport thread.
        std::mutex s_noticesLock;
        std::vector<ChatLine> s_notices;

        void AddNotice(std::string a_text)
        {
            const std::scoped_lock lock{ s_noticesLock };
            s_notices.push_back(ChatLine{ .kind = LineKind::kNotice, .text = std::move(a_text) });
        }

        std::string DescribeRejection(std::string_view a_error)
        {
            if (a_error == "rate limited")
            {
                return "You are sending messages too quickly.";
            }
            if (a_error == "not in world")
            {
                return "You are not in the world, so nobody nearby can hear you; start with ! to talk to everyone.";
            }
            if (a_error == "unknown recipient")
            {
                return "No player with that name is online.";
            }
            if (a_error == "empty whisper")
            {
                return "Type a message after the player's name.";
            }
            return "The server did not accept your message.";
        }

        struct TypedMessage
        {
            Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kLocal };
            std::string_view text;
        };

        TypedMessage ParseTypedMessage(std::string_view a_text)
        {
            TypedMessage message{ .text = Multiplayer::TrimChatText(a_text) };
            if (message.text.starts_with('!'))
            {
                message.channel = Multiplayer::ChatChannel::kGlobal;
            }
            else if (message.text.starts_with('@'))
            {
                message.channel = Multiplayer::ChatChannel::kWhisper;
            }
            else
            {
                return message;
            }
            message.text = Multiplayer::TrimChatText(message.text.substr(1));
            return message;
        }

        std::string FormatSender(const Multiplayer::ChatMessage& a_message)
        {
            const auto format = [](const Multiplayer::ChatSpeaker& a_speaker) {
                return PlayerLabel::Format(a_speaker.name, a_speaker.nameIndex, a_speaker.level);
            };
            if (a_message.channel != Multiplayer::ChatChannel::kWhisper)
            {
                return format(a_message.sender);
            }
            return a_message.outgoing ? "To " + format(a_message.recipient) : "From " + format(a_message.sender);
        }
    }

    void SendLocalMessage(std::string_view a_text)
    {
        const auto message = ParseTypedMessage(a_text);
        if (message.text.empty())
        {
            if (message.channel == Multiplayer::ChatChannel::kWhisper)
            {
                AddNotice("Type a player's name after @, then your message.");
            }
            return;
        }
        if (!Multiplayer::IsValidChatText(message.text))
        {
            AddNotice("Your message contains characters that cannot be sent.");
            return;
        }
        if (!Network::PublishChatMessage(message.channel, message.text,
                [](std::string_view a_error) { AddNotice(DescribeRejection(a_error)); }))
        {
            AddNotice("Not connected to the server; your message was not sent.");
        }
    }

    void DrainLines(std::vector<ChatLine>& a_lines)
    {
        for (auto& message : Network::DrainChatMessages())
        {
            a_lines.push_back(ChatLine{ .kind = LineKind::kPlayer,
                .channel = message.channel,
                .sender = FormatSender(message),
                .text = std::move(message.text) });
        }

        const std::scoped_lock lock{ s_noticesLock };
        for (auto& notice : s_notices)
        {
            a_lines.push_back(std::move(notice));
        }
        s_notices.clear();
    }
}
