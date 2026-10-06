#pragma once

#include <string_view>

namespace UI::ConnectionControls
{
    [[nodiscard]] float GetButtonWidth();

    void DrawButton(bool a_displayNameEntered);

    [[nodiscard]] const char* GetDisplayNameLock();

    void DrawNotice(bool a_invalidText, std::string_view a_help);

    void DrawSummary(float a_width);
}
