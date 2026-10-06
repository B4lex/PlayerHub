#include "sync/chat/ChatSync.h"
#include "network/ServerClient.h"
#include "sync/PlayerLabel.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <mutex>
#include <utility>

namespace ChatSync
{
    namespace
    {
        std::mutex s_noticesLock;
        std::vector<ChatLine> s_notices;

        std::string DescribeRejection(std::string_view a_error)
        {
            if (a_error == "rate limited")
            {
                return "You are sending messages too quickly.";
            }
            if (a_error == "not in world")
            {
                return "You are not in the world, so nobody nearby can hear you; switch to Global to talk to everyone.";
            }
            if (a_error == "unknown recipient")
            {
                return "That player is no longer online.";
            }
            return "The server did not accept your message.";
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
            return a_message.outgoing ? "To " + format(a_message.recipient) : "From " + format(a_message.sender);
        }
    }

    bool Send(const ChatTarget& a_target, std::string_view a_text)
    {
        const auto text = Multiplayer::TrimChatText(a_text);
        if (text.empty())
        {
            return true;
        }
        if (!Multiplayer::IsValidChatText(text))
        {
            AddNotice("Your message contains characters that cannot be sent.");
            return false;
        }
        Multiplayer::PeerSlot recipient = 0;
        if (a_target.channel == Multiplayer::ChatChannel::kWhisper)
        {
            if (!Network::IsConnected())
            {
                AddNotice("Not connected to the server; your message was not sent.");
                return false;
            }
            const auto player = ChatRoster::Resolve(a_target.player);
            if (!player)
            {
                AddNotice(a_target.player.name + " is offline.");
                return false;
            }
            recipient = player->playerID;
        }
        if (!Network::PublishChatMessage(a_target.channel, recipient, text,
                [](std::string_view a_error) { AddNotice(DescribeRejection(a_error)); }))
        {
            AddNotice("Not connected to the server; your message was not sent.");
            return false;
        }
        logs::debug("Chat sent: channel={} recipient={} bytes={}", static_cast<int>(a_target.channel), recipient,
            text.size());
        return true;
    }

    void AddNotice(std::string a_text)
    {
        logs::debug("Chat notice: {}", a_text);
        const std::scoped_lock lock{ s_noticesLock };
        s_notices.push_back(ChatLine{ .kind = LineKind::kNotice, .text = std::move(a_text) });
    }

    void DrainLines(std::vector<ChatLine>& a_lines)
    {
        for (auto& message : Network::DrainChatMessages())
        {
            const bool whisper = message.channel == Multiplayer::ChatChannel::kWhisper;
            const auto& other = whisper && message.outgoing ? message.recipient : message.sender;
            ChatLine line{ .kind = LineKind::kPlayer,
                .channel = message.channel,
                .sender = FormatSender(message),
                .text = std::move(message.text),
                .player = { .playerID = other.playerID, .name = other.name, .session = other.session },
                .outgoing = message.outgoing };
            // A note to oneself is not a conversation, and a muted player's whispers are
            // hidden, so neither changes /r.
            if (whisper && message.sender.playerID != message.recipient.playerID
                && (message.outgoing || !PlayerFilters::IsMuted(message.sender.session)))
            {
                ChatRoster::NoteWhisper(line.player, !message.outgoing);
            }
            a_lines.push_back(std::move(line));
        }

        const std::scoped_lock lock{ s_noticesLock };
        for (auto& notice : s_notices)
        {
            a_lines.push_back(std::move(notice));
        }
        s_notices.clear();
    }
}
