#pragma once

namespace UI::PingIndicator
{
    // Draws the connection state and round-trip latency in a screen corner.
    // Call between ImGui::NewFrame and ImGui::Render.
    void Draw();
}
