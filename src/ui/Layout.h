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

    inline void SetNextWindowCorner(ScreenCorner a_corner, ImVec2 a_margin)
    {
        const auto& display = ImGui::GetIO().DisplaySize;
        const bool right = a_corner == ScreenCorner::kTopRight || a_corner == ScreenCorner::kBottomRight;
        const bool bottom = a_corner == ScreenCorner::kBottomLeft || a_corner == ScreenCorner::kBottomRight;

        ImGui::SetNextWindowPos(
            ImVec2{ right ? display.x - a_margin.x : a_margin.x, bottom ? display.y - a_margin.y : a_margin.y },
            ImGuiCond_Always, ImVec2{ right ? 1.0F : 0.0F, bottom ? 1.0F : 0.0F });
    }

    inline void SetNextWindowCorner(ScreenCorner a_corner, float a_margin)
    {
        SetNextWindowCorner(a_corner, ImVec2{ a_margin, a_margin });
    }
}
