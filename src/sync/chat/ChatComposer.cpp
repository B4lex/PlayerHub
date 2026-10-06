#include "sync/chat/ChatComposer.h"
#include "sync/chat/ChatDraft.h"
#include "sync/chat/ChatRoster.h"

#include <algorithm>
#include <format>
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

        void AddHelp()
        {
            AddNotice("! switches to Global and @name whispers. Tab steps through the channels.");
            for (const auto& command : ChatDraft::GetCommands())
            {
                AddNotice(command.argument.empty()
                              ? std::format("/{} - {}", command.name, command.description)
                              : std::format("/{} {} - {}", command.name, command.argument, command.description));
            }
        }

        void AddPlayerCount()
        {
            const auto count = ChatRoster::GetPlayerCount();
            if (count == 0)
            {
                AddNotice("Not connected to the server.");
            }
            else if (count == 1)
            {
                AddNotice("You are the only player online.");
            }
            else
            {
                AddNotice(std::format("{} players online.", count));
            }
        }
    }

    void Composer::SetTarget(const ChatTarget& a_target)
    {
        if (target.channel != Multiplayer::ChatChannel::kWhisper)
        {
            previousChannel = target.channel;
        }
        target = a_target;
    }

    void Composer::CycleTarget(int a_direction)
    {
        std::vector<ChatTarget> targets{ ChatTarget{ .channel = Multiplayer::ChatChannel::kGlobal },
            ChatTarget{ .channel = Multiplayer::ChatChannel::kLocal } };
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

    bool Composer::LeaveWhisper()
    {
        if (target.channel != Multiplayer::ChatChannel::kWhisper)
        {
            return false;
        }
        target = ChatTarget{ .channel = previousChannel };
        return true;
    }

    bool Composer::OnEdited(std::string& a_draft)
    {
        dismissed = false;
        recalled = sent.size();
        bool rewritten = false;
        for (;;)
        {
            const auto typed = ChatDraft::Interpret(a_draft, false);
            switch (typed.kind)
            {
                case ChatDraft::Kind::kChannel:
                    SetTarget(ChatTarget{ .channel = typed.channel });
                    break;
                case ChatDraft::Kind::kReply:
                    Reply();
                    break;
                case ChatDraft::Kind::kWho:
                    AddPlayerCount();
                    break;
                case ChatDraft::Kind::kHelp:
                    AddHelp();
                    break;
                case ChatDraft::Kind::kWhisper:
                {
                    ListPlayers(typed.rest);
                    const auto addressee
                        = suggestions.empty() ? ChatRoster::FindAddressee(typed.rest) : std::nullopt;
                    if (!addressee || addressee->nameLength == typed.rest.size())
                    {
                        return rewritten;
                    }
                    SetTarget(Whisper(addressee->player));
                    a_draft = std::string{ addressee->text };
                    rewritten = true;
                    continue;
                }
                case ChatDraft::Kind::kIncompleteCommand:
                    if (a_draft.find(' ') == std::string::npos)
                    {
                        ListCommands(typed.rest);
                    }
                    else
                    {
                        ClearListing();
                    }
                    return rewritten;
                default:
                    ClearListing();
                    return rewritten;
            }
            a_draft = std::string{ typed.rest };
            rewritten = true;
        }
    }

    Composer::SubmitResult Composer::Submit(std::string& a_draft)
    {
        if (GetListing() != Listing::kNone && !suggestions.empty())
        {
            AcceptSuggestion(selection, a_draft);
            return SubmitResult::kStayOpen;
        }

        const auto typed = ChatDraft::Interpret(a_draft, true);
        switch (typed.kind)
        {
            case ChatDraft::Kind::kText:
            {
                const auto text = Multiplayer::TrimChatText(typed.rest);
                if (!Send(target, text))
                {
                    return SubmitResult::kStayOpen;
                }
                Remember(text);
                a_draft.clear();
                ClearListing();
                return SubmitResult::kClose;
            }
            case ChatDraft::Kind::kChannel:
                SetTarget(ChatTarget{ .channel = typed.channel });
                break;
            case ChatDraft::Kind::kReply:
                Reply();
                break;
            case ChatDraft::Kind::kWho:
                AddPlayerCount();
                break;
            case ChatDraft::Kind::kHelp:
                AddHelp();
                break;
            case ChatDraft::Kind::kWhisper:
            {
                if (typed.rest.empty())
                {
                    AddNotice("Type a player's name after @, then your message.");
                    return SubmitResult::kStayOpen;
                }
                const auto addressee = ChatRoster::FindAddressee(typed.rest);
                if (!addressee)
                {
                    AddNotice("No player with that name is online.");
                    return SubmitResult::kStayOpen;
                }
                SetTarget(Whisper(addressee->player));
                a_draft = std::string{ addressee->text };
                ClearListing();
                return a_draft.empty() ? SubmitResult::kStayOpen : Submit(a_draft);
            }
            default:
                AddNotice(std::format("Unknown command /{}. Type /help for the list.", typed.rest));
                return SubmitResult::kStayOpen;
        }
        a_draft = std::string{ typed.rest };
        ClearListing();
        return SubmitResult::kStayOpen;
    }

    std::span<const Composer::Suggestion> Composer::GetSuggestions() const
    {
        return dismissed ? std::span<const Suggestion>{} : std::span<const Suggestion>{ suggestions };
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
        const auto accepted = suggestions[a_index];
        if (accepted.command.empty())
        {
            SetTarget(Whisper(accepted.player));
            a_draft.clear();
            ClearListing();
            return;
        }
        a_draft = std::format("/{} ", accepted.command);
        OnEdited(a_draft);
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
        players.clear();
        ChatRoster::Complete(a_prefix, players, kMaximumSuggestions);
        suggestions.clear();
        for (auto& player : players)
        {
            suggestions.push_back(Suggestion{ .label = player.name, .player = std::move(player) });
        }
        listing = Listing::kPlayers;
        selection = 0;
    }

    void Composer::ListCommands(std::string_view a_prefix)
    {
        suggestions.clear();
        for (const auto& command : ChatDraft::GetCommands())
        {
            if (command.name.starts_with(a_prefix))
            {
                suggestions.push_back(Suggestion{ .label = command.argument.empty()
                                                              ? std::format("/{}", command.name)
                                                              : std::format("/{} {}", command.name, command.argument),
                    .detail = std::string{ command.description },
                    .command = command.name });
            }
        }
        listing = suggestions.empty() ? Listing::kNone : Listing::kCommands;
        selection = 0;
    }

    void Composer::ClearListing()
    {
        listing = Listing::kNone;
        suggestions.clear();
        selection = 0;
    }

    void Composer::Reply()
    {
        if (const auto whisperer = ChatRoster::GetLastWhisperer())
        {
            SetTarget(Whisper(*whisperer));
        }
        else
        {
            AddNotice("Nobody has whispered you yet.");
        }
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
