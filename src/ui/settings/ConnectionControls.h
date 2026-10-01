#pragma once

#include <string_view>

namespace UI::ConnectionControls
{
    // The width DrawButton takes whatever its label, so a line can leave room
    // for it.
    [[nodiscard]] float GetButtonWidth();

    // At the cursor, a button that connects, with the accent fill; cancels a
    // pending connection; or disconnects, in red. It follows
    // Network::GetConnectionState, so its label never runs ahead of the
    // transport. It connects only while a_displayNameEntered: the display
    // name's field holds one, which is applied by the time a press lands.
    // Nothing else in the window connects.
    void DrawButton(bool a_displayNameEntered);

    // Why the display name's field is locked, as a hint for the player, or
    // null while disconnected: a session takes its display name only when it
    // connects.
    [[nodiscard]] const char* GetDisplayNameLock();

    // At the cursor and wrapped at the content region's right edge, what
    // stands between the display name and a connection, in red: that the
    // field's text is not a display name (a_invalidText), or why the server
    // refused the latest connection. Otherwise a_help, muted.
    void DrawNotice(bool a_invalidText, std::string_view a_help);

    // The indicator and the status on the current line, with the ping ending
    // a_width from where the line starts.
    void DrawSummary(float a_width);
}
