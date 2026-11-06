#pragma once

struct ImGuiStyle;

namespace UI::Cursor
{
    // Before ImGui::NewFrame, after the style's scale is set: places tooltips clear of the
    // normal cursor.
    void PlaceTooltips(ImGuiStyle& a_style);

    // After the windows draw and before ImGui::Render: draws the cursor the frame asked
    // for at the mouse position, on top of every window.
    void Draw();
}
