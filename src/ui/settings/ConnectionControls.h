#pragma once

namespace UI::ConnectionControls
{
    // Width of what Draw adds to the current line, so a field before it can
    // leave room.
    [[nodiscard]] float GetWidth();

    // A green or red connection indicator, with details on hover, and a button
    // that connects to the live server endpoint, cancels a pending connection,
    // or disconnects. The button follows Network::GetConnectionState, so its
    // label never runs ahead of the transport, and cannot connect until a
    // nickname is applied.
    void Draw();
}
