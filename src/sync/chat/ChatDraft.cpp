#include "sync/chat/ChatDraft.h"

#include <algorithm>
#include <array>

namespace ChatDraft
{
    namespace
    {
        constexpr std::array kCommands{
            Command{ "n", "", "Talk to players nearby" },
            Command{ "g", "", "Talk to everyone" },
            Command{ "w", "name", "Whisper to a player" },
            Command{ "r", "", "Whisper back to the last player who whispered you" },
            Command{ "who", "", "Show how many players are online" },
            Command{ "help", "", "List these commands" },
        };

        struct Alias
        {
            std::string_view name;
            Kind kind;
            Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kNearby };
        };

        constexpr std::array kAliases{
            Alias{ "n", Kind::kChannel, Multiplayer::ChatChannel::kNearby },
            Alias{ "nearby", Kind::kChannel, Multiplayer::ChatChannel::kNearby },
            Alias{ "g", Kind::kChannel, Multiplayer::ChatChannel::kGlobal },
            Alias{ "global", Kind::kChannel, Multiplayer::ChatChannel::kGlobal },
            Alias{ "w", Kind::kWhisper, Multiplayer::ChatChannel::kWhisper },
            Alias{ "whisper", Kind::kWhisper, Multiplayer::ChatChannel::kWhisper },
            Alias{ "r", Kind::kReply, Multiplayer::ChatChannel::kWhisper },
            Alias{ "reply", Kind::kReply, Multiplayer::ChatChannel::kWhisper },
            Alias{ "who", Kind::kWho },
            Alias{ "help", Kind::kHelp },
        };

        bool EqualsWithoutCase(std::string_view a_typed, std::string_view a_name)
        {
            return std::ranges::equal(a_typed, a_name, [](char a_left, char a_right) {
                return (a_left >= 'A' && a_left <= 'Z' ? a_left + ('a' - 'A') : a_left) == a_right;
            });
        }

        std::string_view TrimLeft(std::string_view a_text)
        {
            const auto first = a_text.find_first_not_of(' ');
            return first == std::string_view::npos ? std::string_view{} : a_text.substr(first);
        }
    }

    Interpretation Interpret(std::string_view a_draft, bool a_submitted)
    {
        const auto draft = TrimLeft(a_draft);
        if (draft.starts_with('@'))
        {
            return { Kind::kWhisper, Multiplayer::ChatChannel::kWhisper, TrimLeft(draft.substr(1)) };
        }
        if (!draft.starts_with('/'))
        {
            return { Kind::kText, Multiplayer::ChatChannel::kNearby, draft };
        }

        const auto space = draft.find(' ');
        const auto name = draft.substr(1, space == std::string_view::npos ? space : space - 1);
        if (space == std::string_view::npos && !a_submitted)
        {
            return { Kind::kIncompleteCommand, Multiplayer::ChatChannel::kNearby, name };
        }
        const auto rest = space == std::string_view::npos ? std::string_view{} : TrimLeft(draft.substr(space));
        for (const auto& alias : kAliases)
        {
            if (EqualsWithoutCase(name, alias.name))
            {
                return { alias.kind, alias.channel, rest };
            }
        }
        // Not reported while it is typed: more text may still follow.
        return { a_submitted ? Kind::kUnknownCommand : Kind::kIncompleteCommand, Multiplayer::ChatChannel::kNearby, name };
    }

    std::span<const Command> GetCommands()
    {
        return kCommands;
    }
}
