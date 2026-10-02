#pragma once

#include "protocol/ChatMessage.h"

#include <span>
#include <string_view>

// What the text in the chat input means before it is sent: a prefix or a
// command that changes where the line goes, or plain text. Syntax only; names
// are resolved by the caller.
namespace ChatDraft
{
    enum class Kind
    {
        // Text for the current target.
        kText,
        // "!", "/l", or "/g": the channel changes and the rest stays.
        kChannel,
        // "@" or "/w": the rest starts with a player's name.
        kWhisper,
        // "/r": the rest goes to the last player who whispered.
        kReply,
        kWho,
        kHelp,
        kUnknownCommand,
        // A command still being typed; nothing to do yet.
        kIncompleteCommand
    };

    struct Interpretation
    {
        Kind kind{ Kind::kText };
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kLocal };
        // Without leading spaces. For an unknown or incomplete command, the
        // command's name as typed, without the slash.
        std::string_view rest;
    };

    // A command takes effect once a space follows it, or when a_submitted, on
    // Enter; "!" and "@" at once.
    [[nodiscard]] Interpretation Interpret(std::string_view a_draft, bool a_submitted);

    struct Command
    {
        // Without the slash.
        std::string_view name;
        std::string_view argument;
        std::string_view description;
    };

    [[nodiscard]] std::span<const Command> GetCommands();
}
