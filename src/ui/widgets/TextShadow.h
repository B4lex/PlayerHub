#pragma once

#include <imgui.h>

#include <span>

namespace UI::TextShadow
{
    // How far the shadow reaches past what it is under, at the current font size.
    [[nodiscard]] float GetReach();

    [[nodiscard]] std::span<const ImVec2> GetOffsetsFonts();

    // Turns the vertices from a_firstVertex on into a shadow copy moved by a_offset.
    void Darken(int a_firstVertex, const ImVec2& a_offset);

    // Draws a_draw once per offset as its shadow and then over it, each from the same cursor position.
    template <class DrawFunction>
    void Draw(DrawFunction&& a_draw)
    {
        const auto start = ImGui::GetCursorPos();
        const auto fontSize = ImGui::GetFontSize();
        for (const auto& offset : GetOffsetsFonts())
        {
            const auto firstVertex = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            a_draw();
            Darken(firstVertex, ImVec2{ offset.x * fontSize, offset.y * fontSize });
            ImGui::SetCursorPos(start);
        }
        a_draw();
    }
}
