#pragma once

#include <imgui.h>

namespace UI::CommunityLinks
{
    [[nodiscard]] bool IsEnabled();

    [[nodiscard]] ImVec2 GetSize();

    void Draw();
}
