#pragma once

#include <imgui.h>

namespace UI::DiscordLink
{
    [[nodiscard]] bool IsEnabled();

    [[nodiscard]] ImVec2 GetSize();

    void Draw();
}
