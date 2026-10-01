#pragma once

namespace UI::ConnectionControls
{
    // On the current line, a button that connects to the live server endpoint,
    // cancels a pending connection, or disconnects, then a green or red
    // indicator and the connection status. Below it, while there is one, why
    // the server refused the connection or the applied nickname, or that a
    // nickname is needed to connect. The button follows
    // Network::GetConnectionState, so its label never runs ahead of the
    // transport, and cannot connect until a nickname is applied.
    void Draw();
}
