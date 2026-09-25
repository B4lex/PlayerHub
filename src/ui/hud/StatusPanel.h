#pragma once

namespace UI::StatusPanel
{
    // Draws the connection state, round-trip latency, and the plugin's average
    // frame time in a screen corner.
    // Call between ImGui::NewFrame and ImGui::Render.
    void Draw();
}
