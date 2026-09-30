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
    };

    // Sends text typed by the local player: "!text" to everyone, "@Name text"
    // to one player, and anything else to players nearby. Surrounding spaces
    // are trimmed. The server echoes an accepted message to the sender too, so
    // it arrives through DrainLines. A message that cannot be delivered
    // produces a notice instead. Safe to call from any thread.
    void SendLocalMessage(std::string_view a_text);

    // Appends every line that arrived since the previous call, oldest first.
    // Safe to call from any thread.
    void DrainLines(std::vector<ChatLine>& a_lines);
}
