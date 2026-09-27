#pragma once

#include <string>
#include <string_view>
#include <vector>

// Global text chat. Independent of the world: it runs outside the main-loop
// sync stages and needs no engine access.
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
        // Empty for notices.
        std::string sender;
        std::string text;
    };

    // Sends text typed by the local player; surrounding spaces are trimmed. The
    // server echoes an accepted message to everyone, the sender included, so it
    // arrives through DrainLines. A message that cannot be delivered produces a
    // notice instead. Safe to call from any thread.
    void SendLocalMessage(std::string_view a_text);

    // Appends every line that arrived since the previous call, oldest first.
    // Safe to call from any thread.
    void DrainLines(std::vector<ChatLine>& a_lines);
}
