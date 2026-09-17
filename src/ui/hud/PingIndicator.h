#pragma once

namespace UI::PingIndicator
{
	// Draws the connection state, round-trip latency, and remote packet delay in
	// a screen corner.
	// Call between ImGui::NewFrame and ImGui::Render.
	void Draw();
}
