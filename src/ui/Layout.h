#pragma once

#include <imgui.h>

namespace UI
{
    enum class ScreenCorner
    {
        kTopLeft,
        kTopRight,
        kBottomLeft,
        kBottomRight
    };

    // Anchors the next ImGui window to a_corner, a_margin pixels from both edges.
    inline void SetNextWindowCorner(ScreenCorner a_corner, float a_margin)
    {
        const auto& display = ImGui::GetIO().DisplaySize;
        const bool right = a_corner == ScreenCorner::kTopRight || a_corner == ScreenCorner::kBottomRight;
        const bool bottom = a_corner == ScreenCorner::kBottomLeft || a_corner == ScreenCorner::kBottomRight;

        ImGui::SetNextWindowPos(
            ImVec2{ right ? display.x - a_margin : a_margin, bottom ? display.y - a_margin : a_margin },
            ImGuiCond_Always, ImVec2{ right ? 1.0F : 0.0F, bottom ? 1.0F : 0.0F });
    }
}
