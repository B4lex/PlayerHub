#include "sync/chat/ChatComposer.h"
#include "sync/chat/ChatRoster.h"
#include "text/Text.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace ChatSync
{
    namespace
    {
        constexpr std::size_t kMaximumSuggestions = 8;
        constexpr std::size_t kMaximumCycledPartners = 3;
        constexpr std::size_t kMaximumSentLines = 50;

        ChatTarget Whisper(const Multiplayer::RosterEntry& a_player)
        {
            return ChatTarget{ .channel = Multiplayer::ChatChannel::kWhisper, .player = a_player };
        }

        std::optional<std::string_view> AfterWhisperPrefix(std::string_view a_draft)
        {
            const auto prefix = a_draft.find_first_not_of(' ');
            if (prefix == std::string_view::npos || a_draft[prefix] != '@')
            {
                return std::nullopt;
            }
            const auto text = a_draft.find_first_not_of(' ', prefix + 1);
            return text == std::string_view::npos ? std::string_view{} : a_draft.substr(text);
        }
    }

    void Composer::SetTarget(const ChatTarget& a_target)
    {
        notes.clear();
        target = a_target;
    }

    void Composer::CycleTarget(int a_direction)
    {
        std::vector<ChatTarget> targets{ ChatTarget{ .channel = Multiplayer::ChatChannel::kGlobal },
            ChatTarget{ .channel = Multiplayer::ChatChannel::kNearby } };
        players.clear();
        ChatRoster::GetRecentPartners(players, kMaximumCycledPartners);
        // A player picked from the list is not a partner before the first whisper.
        if (target.channel == Multiplayer::ChatChannel::kWhisper
            && std::ranges::none_of(players, [&](const auto& a_player) { return Whisper(a_player) == target; }))
        {
            targets.push_back(target);
        }
        for (const auto& player : players)
        {
            targets.push_back(Whisper(player));
        }
        const auto current = std::ranges::find(targets, target);
        const auto index = static_cast<std::ptrdiff_t>(current == targets.end() ? 0 : current - targets.begin());
        const auto count = static_cast<std::ptrdiff_t>(targets.size());
        SetTarget(targets[static_cast<std::size_t>(((index + a_direction) % count + count) % count)]);
    }

    bool Composer::OnEdited(std::string& a_draft)
    {
        dismissed = false;
        recalled = sent.size();
        notes.clear();
        bool rewritten = false;
        for (;;)
        {
            const auto typed = AfterWhisperPrefix(a_draft);
            if (!typed)
            {
                ClearListing();
                return rewritten;
            }
            ListPlayers(*typed);
            const auto addressee = suggestions.empty() ? ChatRoster::FindAddressee(*typed) : std::nullopt;
            if (!addressee || addressee->nameLength == typed->size())
            {
                return rewritten;
            }
            SetTarget(Whisper(addressee->player));
            a_draft = std::string{ addressee->text };
            rewritten = true;
        }
    }

    Composer::SubmitResult Composer::Submit(std::string& a_draft)
    {
        notes.clear();
        if (IsListing() && !suggestions.empty())
        {
            AcceptSuggestion(selection, a_draft);
            return SubmitResult::kStayOpen;
        }

        if (const auto typed = AfterWhisperPrefix(a_draft))
        {
            if (typed->empty())
            {
                Fail(std::string{ Text::Get(Text::Id::kChatWhisperNeedsName) });
                return SubmitResult::kStayOpen;
            }
            const auto addressee = ChatRoster::FindAddressee(*typed);
            if (!addressee)
            {
                Fail(std::string{ Text::Get(Text::Id::kChatNoPlayerNamed) });
                return SubmitResult::kStayOpen;
            }
            SetTarget(Whisper(addressee->player));
            a_draft = std::string{ addressee->text };
            ClearListing();
            return a_draft.empty() ? SubmitResult::kStayOpen : Submit(a_draft);
        }

        const auto text = Multiplayer::TrimChatText(a_draft);
        if (const auto result = Send(target, text); !result)
        {
            Fail(result.error());
            return SubmitResult::kStayOpen;
        }
        Remember(text);
        a_draft.clear();
        ClearListing();
        return SubmitResult::kClose;
    }

    std::span<const Multiplayer::RosterEntry> Composer::GetSuggestions() const
    {
        return dismissed ? std::span<const Multiplayer::RosterEntry>{} : std::span{ suggestions };
    }

    void Composer::MoveSelection(int a_direction)
    {
        if (suggestions.empty())
        {
            return;
        }
        const auto count = static_cast<std::ptrdiff_t>(suggestions.size());
        selection = static_cast<std::size_t>(
            ((static_cast<std::ptrdiff_t>(selection) + a_direction) % count + count) % count);
    }

    void Composer::AcceptSuggestion(std::size_t a_index, std::string& a_draft)
    {
        if (a_index >= suggestions.size())
        {
            return;
        }
        SetTarget(Whisper(suggestions[a_index]));
        a_draft.clear();
        ClearListing();
    }

    bool Composer::RecallPrevious(std::string& a_draft)
    {
        recalled = (std::min)(recalled, sent.size());
        if (recalled == 0)
        {
            return false;
        }
        if (recalled == sent.size())
        {
            unsent = Sent{ .target = target, .text = a_draft };
        }
        --recalled;
        SetTarget(sent[recalled].target);
        a_draft = sent[recalled].text;
        ClearListing();
        return true;
    }

    bool Composer::RecallNext(std::string& a_draft)
    {
        if (recalled >= sent.size())
        {
            return false;
        }
        ++recalled;
        const auto& line = recalled == sent.size() ? unsent : sent[recalled];
        SetTarget(line.target);
        a_draft = line.text;
        ClearListing();
        return true;
    }

    void Composer::ListPlayers(std::string_view a_prefix)
    {
        suggestions.clear();
        ChatRoster::Complete(a_prefix, suggestions, kMaximumSuggestions);
        listing = true;
        selection = 0;
    }

    void Composer::ClearListing()
    {
        listing = false;
        suggestions.clear();
        selection = 0;
    }

    void Composer::Fail(std::string a_text)
    {
        notes.push_back(std::move(a_text));
    }

    void Composer::Remember(std::string_view a_text)
    {
        if (!a_text.empty() && (sent.empty() || sent.back().text != a_text || !(sent.back().target == target)))
        {
            if (sent.size() >= kMaximumSentLines)
            {
                sent.pop_front();
            }
            sent.push_back(Sent{ .target = target, .text = std::string{ a_text } });
        }
        recalled = sent.size();
    }
}
