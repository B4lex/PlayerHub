#pragma once

#include "protocol/ChatMessage.h"

#include <string>
#include <string_view>
#include <vector>

// Text chat on three channels: global, local, and whispers. Independent of the
// world: it runs outside the main-loop sync stages and needs no engine access.
namespace ChatSync
{
    enum class LineKind
    {
        kPlayer,
        // Local feedback, such as a message that could not be sent.
        kNotice
    };

    struct ChatLine
    {
        LineKind kind{ LineKind::kPlayer };
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
        // Empty for notices. A whisper's reads "To X" or "From X".
        std::string sender;
        std::string text;
        // The other player of the line: its sender, or the recipient of the
        // local player's own whisper. No slot for a notice.
        Multiplayer::RosterEntry player;
        // The local player sent it.
        bool outgoing{ false };
    };

    // Where a line goes: a channel, and for a whisper the player.
    struct ChatTarget
    {
        Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kLocal };
        Multiplayer::RosterEntry player;

        [[nodiscard]] bool operator==(const ChatTarget& a_other) const
        {
            // A player who connected again has another slot and the same name.
            return channel == a_other.channel
                && (channel != Multiplayer::ChatChannel::kWhisper || player.playerID == a_other.player.playerID
                    || player.name == a_other.player.name);
        }
    };

    // Sends text typed by the local player, with surrounding spaces trimmed.
    // The server echoes an accepted message to the sender too, so it arrives
    // through DrainLines. Returns false with a notice when the message was not
    // handed to the server: no connection, a whisper's player gone, or text
    // that cannot be sent. Empty text sends nothing and returns true. Safe to
    // call from any thread.
    bool Send(const ChatTarget& a_target, std::string_view a_text);

    // Adds a notice line. Safe to call from any thread.
    void AddNotice(std::string a_text);

    // Appends every line that arrived since the previous call, oldest first,
    // and records whispers with ChatRoster. Safe to call from any thread.
    void DrainLines(std::vector<ChatLine>& a_lines);
}
