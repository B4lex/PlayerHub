#include "sync/chat/ChatSync.h"
#include "network/ServerClient.h"
#include "sync/PlayerLabel.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"
#include "text/Text.h"

#include <mutex>
#include <utility>

namespace ChatSync
{
    namespace
    {
        std::mutex s_rejectionsLock;
        std::vector<std::string> s_rejections;

        std::string DescribeRejection(Multiplayer::ChatRejection a_rejection)
        {
            switch (a_rejection)
            {
                case Multiplayer::ChatRejection::kRateLimited:
                    return std::string{ Text::Get(Text::Id::kChatRateLimited) };
                case Multiplayer::ChatRejection::kNotInWorld:
                    return std::string{ Text::Get(Text::Id::kChatNotInWorld) };
                case Multiplayer::ChatRejection::kUnknownRecipient:
                    return std::string{ Text::Get(Text::Id::kChatRecipientGone) };
                default:
                    return std::string{ Text::Get(Text::Id::kChatRejected) };
            }
        }

        struct SenderLabel
        {
            std::string text;
            std::size_t nameOffset{ 0 };
        };

        SenderLabel FormatSender(const Multiplayer::ChatMessage& a_message)
        {
            if (a_message.channel != Multiplayer::ChatChannel::kWhisper)
            {
                return SenderLabel{ .text = PlayerLabel::Format(a_message.sender.name) };
            }
            const auto format = a_message.outgoing ? Text::Id::kChatTo : Text::Id::kChatFrom;
            const auto& other = a_message.outgoing ? a_message.recipient : a_message.sender;
            // A translation may put the name anywhere in the label.
            const auto field = Text::Get(format).find("{}");
            return SenderLabel{ .text = Text::Format(format, PlayerLabel::Format(other.name)),
                .nameOffset = field != std::string_view::npos ? field : 0 };
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
            return std::unexpected{ std::string{ Text::Get(Text::Id::kChatInvalidText) } };
        }
        Multiplayer::PeerSlot recipient = 0;
        if (a_target.channel == Multiplayer::ChatChannel::kWhisper)
        {
            if (!Network::IsConnected())
            {
                return std::unexpected{ std::string{ Text::Get(Text::Id::kNotConnected) } };
            }
            const auto player = ChatRoster::Resolve(a_target.player);
            if (!player)
            {
                return std::unexpected{ Text::Format(Text::Id::kChatPlayerOffline, a_target.player.name) };
            }
            recipient = player->playerID;
        }
        if (!Network::PublishChatMessage(a_target.channel, recipient, text, [](Multiplayer::ChatRejection a_rejection) {
                auto rejection = DescribeRejection(a_rejection);
                const std::scoped_lock lock{ s_rejectionsLock };
                s_rejections.push_back(std::move(rejection));
            }))
        {
            return std::unexpected{ std::string{ Text::Get(Text::Id::kNotConnected) } };
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
            auto sender = FormatSender(message);
            ChatLine line{ .channel = message.channel,
                .sender = std::move(sender.text),
                .nameOffset = sender.nameOffset,
                .text = std::move(message.text),
                .player = other,
                .outgoing = message.outgoing };
            ChatRoster::FindMentions(line.text, line.mentions);
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
