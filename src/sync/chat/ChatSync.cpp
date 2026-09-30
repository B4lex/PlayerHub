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
            return "The server did not accept your message.";
        }
    }

    void SendLocalMessage(std::string_view a_text)
    {
        const auto text = Multiplayer::TrimChatText(a_text);
        if (text.empty())
        {
            return;
        }
        if (!Multiplayer::IsValidChatText(text))
        {
            AddNotice("Your message contains characters that cannot be sent.");
            return;
        }
        if (!Network::PublishChatMessage(
                text, [](std::string_view a_error) { AddNotice(DescribeRejection(a_error)); }))
        {
            AddNotice("Not connected to the server; your message was not sent.");
        }
    }

    void DrainLines(std::vector<ChatLine>& a_lines)
    {
        for (auto& message : Network::DrainChatMessages())
        {
            a_lines.push_back(ChatLine{ .kind = LineKind::kPlayer,
                .sender = PlayerLabel::Format(message.senderName, message.senderNameIndex, message.senderLevel),
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
