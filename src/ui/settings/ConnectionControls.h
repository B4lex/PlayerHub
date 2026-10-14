#pragma once

namespace UI::ConnectionControls
{
    [[nodiscard]] float GetButtonWidth();

    [[nodiscard]] bool CanConnect(bool a_displayNameEntered);

    void Connect();

    void DrawButton(bool a_displayNameEntered);

    [[nodiscard]] const char* GetDisplayNameLock();

    [[nodiscard]] const char* GetButtonLock();

    void DrawNotice(bool a_shortText);

    void DrawSummary(float a_width);
}
