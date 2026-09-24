#pragma once

#include <chrono>
#include <optional>

namespace Network
{
    // Main-loop thread, once per pass with the frame delta. While connected it
    // probes the server and abandons a connection whose probe goes unanswered;
    // while disconnected, for any reason, it keeps reopening the connection.
    void UpdateConnectionMonitor(float a_deltaSeconds);

    // Round trip of the latest acknowledged probe. Empty while disconnected or
    // before the first acknowledgement of this connection. Safe to call from any
    // thread.
    [[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();
}
