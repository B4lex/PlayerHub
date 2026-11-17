#include "sync/chat/ChatComposer.h"
#include "protocol/Session.h"
#include "sync/chat/ChatRoster.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ChatSync
{
    namespace
    {
        constexpr std::size_t kMaximumSuggestions = 8;
        constexpr std::size_t kMaximumCycledPartners = 3;
        constexpr std::size_t kMaximumSentLines = 50;

        ChatTarget Whisper(const Multiplayer::PlayerIdentity& a_player)
        {
            return ChatTarget{ .channel = Multiplayer::ChatChannel::kWhisper, .player = a_player };
        }

        bool IsNameCharacter(std::string_view a_text, std::size_t a_index)
        {
            return a_index < a_text.size() && Multiplayer::IsDisplayNameCharacter(a_text[a_index]);
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

    void Composer::OnEdited()
    {
        dismissed = false;
        recalled = sent.size();
        notes.clear();
        ClearListing();
        stale = true;
    }

    void Composer::Update(std::string_view a_draft, std::size_t a_caret)
    {
        a_caret = (std::min)(a_caret, a_draft.size());
        if (!stale && a_caret == updatedCaret)
        {
            return;
        }
        stale = false;
        updatedCaret = a_caret;

        // As ChatRoster::FindMentions reads them: an `@` that does not end a word, then name characters.
        auto start = a_caret;
        while (start > 0 && IsNameCharacter(a_draft, start - 1))
        {
            --start;
        }
        if (start == 0 || a_draft[start - 1] != '@' || (start > 1 && IsNameCharacter(a_draft, start - 2)))
        {
            ClearListing();
            return;
        }
        auto end = a_caret;
        while (IsNameCharacter(a_draft, end))
        {
            ++end;
        }
        const auto prefix = a_draft.substr(start, a_caret - start);
        if (mention && mention->offset == start - 1 && mention->prefix == prefix)
        {
            mention->end = end;
            return;
        }
        mention = TypedMention{ .offset = start - 1, .end = end, .prefix = std::string{ prefix } };
        suggestions.clear();
        ChatRoster::Complete(prefix, suggestions, kMaximumSuggestions);
        // A name typed in full has nothing left to pick, so Enter sends.
        if (suggestions.size() == 1 && ChatRoster::FoldName(suggestions.front().name) == ChatRoster::FoldName(prefix))
        {
            suggestions.clear();
        }
        selection = 0;
    }

    Composer::SubmitResult Composer::Submit(std::string& a_draft)
    {
        notes.clear();
        if (IsListing())
        {
            AcceptSuggestion(selection, a_draft);
            return SubmitResult::kStayOpen;
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
        stale = true;
        return SubmitResult::kClose;
    }

    std::span<const Multiplayer::PlayerIdentity> Composer::GetSuggestions() const
    {
        return dismissed ? std::span<const Multiplayer::PlayerIdentity>{} : std::span{ suggestions };
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

    bool Composer::AcceptSuggestion(std::size_t a_index, std::string& a_draft)
    {
        if (a_index >= suggestions.size() || !mention || mention->end > a_draft.size())
        {
            return false;
        }
        const auto& name = suggestions[a_index].name;
        const bool spaced = mention->end < a_draft.size() && a_draft[mention->end] == ' ';
        auto written = a_draft.substr(0, mention->offset) + '@' + name;
        const auto caret = written.size() + 1;
        written.append(spaced ? "" : " ").append(a_draft, mention->end);
        if (Multiplayer::CountChatCharacters(written) > Multiplayer::kMaximumChatTextCharacters)
        {
            DismissListing();
            return false;
        }
        a_draft = std::move(written);
        placedCaret = caret;
        ClearListing();
        stale = true;
        return true;
    }

    std::optional<std::size_t> Composer::TakePlacedCaret()
    {
        return std::exchange(placedCaret, std::nullopt);
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
        Recalled(a_draft);
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
        Recalled(a_draft);
        return true;
    }

    void Composer::Recalled(std::string_view a_draft)
    {
        // A recalled line ending in a name does not list players, so Up and Down keep recalling.
        ClearListing();
        stale = false;
        updatedCaret = a_draft.size();
    }

    void Composer::ClearListing()
    {
        mention.reset();
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
