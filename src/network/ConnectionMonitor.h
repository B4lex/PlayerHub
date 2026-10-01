#pragma once

#include "network/Refusal.h"

#include <chrono>
#include <optional>
#include <string>

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
    // (GameSession), while the player's choice enables it, and once
    // [General] Nickname is set. While one is wanted and open, the monitor
    // probes the server and abandons a connection whose probe goes unanswered,
    // and asks the server to rename the session when the nickname changes;
    // while one is wanted but lost, for any reason, it keeps reopening it.
    // Otherwise it closes the client.
    void UpdateConnectionMonitor(float a_deltaSeconds);

    // The player's choice in the settings window. Each game starts with
    // [Network] AutoConnect, and the choice lasts until the game ends. Safe to
    // call from any thread.
    void SetConnectionEnabled(bool a_enabled);

    // Whether [General] Nickname is set, as of the monitor's latest pass. Without
    // one no connection is wanted, whatever the player's choice. Safe to call
    // from any thread.
    [[nodiscard]] bool HasNickname();

    // Whether a connection is wanted combined with what the transport reports,
    // so it never shows a connection as open or closed before it is. Safe to
    // call from any thread.
    [[nodiscard]] ConnectionState GetConnectionState();

    // Why the server refused this client, while a connection is wanted but will
    // not be retried. A refused nickname is retried once another one is applied.
    // Empty otherwise. Safe to call from any thread.
    [[nodiscard]] std::optional<Refusal> GetConnectionRefusal();

    // Why the server did not rename the session to [General] Nickname while
    // connected, as a sentence for the player; peers still see the previous
    // nickname. Empty once a rename succeeds or the connection ends. Safe to
    // call from any thread.
    [[nodiscard]] std::optional<std::string> GetNicknameRejection();

    // Round trip of the latest acknowledged probe. Empty while disconnected or
    // before the first acknowledgement of this connection. Safe to call from any
    // thread.
    [[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();
}
