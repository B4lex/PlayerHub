#pragma once

#include <cstddef>
#include <optional>

namespace Network
{
    struct TrafficRates
    {
        double receivedBytesPerSecond = 0.0;
        double sentBytesPerSecond = 0.0;
    };

    // Adds the estimated size of one Socket.IO packet. WebSocket framing, TLS,
    // and Engine.IO heartbeats are not visible to the client, so they are not
    // counted. Safe to call from any thread.
    void CountReceivedBytes(std::size_t a_bytes);
    void CountSentBytes(std::size_t a_bytes);

    // Average rates over the latest complete one-second window, measured in
    // wall-clock time. Empty until the first window completes, and again for
    // one window after a long gap between calls. Safe to call from any thread.
    [[nodiscard]] std::optional<TrafficRates> GetTrafficRates();
}
