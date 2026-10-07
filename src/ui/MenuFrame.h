#pragma once

#include <imgui.h>

namespace UI::MenuFrame
{
    void Load();

    struct Metrics
    {
        float inset{ 0.0F };
        float corner{ 0.0F };
        float rounding{ 0.0F };
    };
    [[nodiscard]] Metrics GetMetrics();

    // The anti-aliased fill of pieces this fine overlaps its own triangles, which
    // a translucent line color shows as brighter specks.
    void Draw(ImU32 a_line, ImU32 a_backing);

    struct WindowLayout
    {
        ImVec2 sizeFonts;
        ImVec2 minimumSizeFonts;
        // Added to the frame's inset.
        float paddingFonts{ 0.0F };
        float backgroundAlpha{ 1.0F };
    };

    // Begins a resizable window, centered and focused when it appears or when
    // a_focus is set, inside the menu frame. Call ImGui::End whatever it returns.
    [[nodiscard]] bool BeginWindow(
        const char* a_name, const WindowLayout& a_layout, ImGuiWindowFlags a_flags, bool a_focus);
}
