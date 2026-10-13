#pragma once

namespace UI::ConnectionControls
{
    [[nodiscard]] float GetButtonWidth();

    void DrawButton(bool a_displayNameEntered);

    [[nodiscard]] const char* GetDisplayNameLock();

    [[nodiscard]] const char* GetButtonLock();

    void DrawNotice(bool a_invalidText);

    void DrawSummary(float a_width);
}
