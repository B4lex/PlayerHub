#pragma once

#include "protocol/ChatMessage.h"

#include <span>
#include <string_view>

namespace ChatDraft
{
    enum class Kind
    {
        kText,
        kChannel,
        kWhisper,
        kReply,
        kWho,
        kHelp,
        kUnknownCommand,
        kIncompleteCommand
    };

    struct Interpretation
    {
        Kind kind{ Kind::kText };
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kLocal };
        std::string_view rest;
    };

    [[nodiscard]] Interpretation Interpret(std::string_view a_draft, bool a_submitted);

    struct Command
    {
        std::string_view name;
        std::string_view argument;
        std::string_view description;
    };

    [[nodiscard]] std::span<const Command> GetCommands();
}
