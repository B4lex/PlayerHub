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

    // Adds the size of one WebSocket message as IXWebSocket reports it on the
    // wire. TLS, TCP, and ping frames are not counted. Safe to call from any
    // thread.
    void CountReceivedBytes(std::size_t a_bytes);
    void CountSentBytes(std::size_t a_bytes);

    // Average rates over the latest complete one-second window, measured in
    // wall-clock time. Empty until the first window completes, and again for
    // one window after a long gap between calls. Safe to call from any thread.
    [[nodiscard]] std::optional<TrafficRates> GetTrafficRates();
}
