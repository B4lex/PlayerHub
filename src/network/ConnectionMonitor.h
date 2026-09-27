#pragma once

#include <chrono>
#include <optional>

namespace Network
{
    enum class ConnectionState
    {
        kDisconnected,
        // No connection is wanted, but the transport has not closed yet.
        kDisconnecting,
        // A connection is wanted but not open, including while a previous
        // close still holds up the next attempt.
        kConnecting,
        kConnected,
    };

    // Main-loop thread, once per pass with the frame delta, which is zero while
    // the game is paused. A connection is wanted only in a game
    // (GameSession) and while the player's choice enables it. While one is wanted and open, the monitor
    // probes the server and abandons a connection whose probe goes unanswered;
    // while one is wanted but lost, for any reason, it keeps reopening it.
    // Otherwise it closes the client.
    void UpdateConnectionMonitor(float a_deltaSeconds);

    // The player's choice in the settings window. Each game starts with
    // [Network] AutoConnect, and the choice lasts until the game ends. Safe to
    // call from any thread.
    void SetConnectionEnabled(bool a_enabled);

    // Whether a connection is wanted combined with what the transport reports,
    // so it never shows a connection as open or closed before it is. Safe to
    // call from any thread.
    [[nodiscard]] ConnectionState GetConnectionState();

    // Round trip of the latest acknowledged probe. Empty while disconnected or
    // before the first acknowledgement of this connection. Safe to call from any
    // thread.
    [[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();
}
