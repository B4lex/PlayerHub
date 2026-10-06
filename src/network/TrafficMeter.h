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

    // IXWebSocket's wire size leaves out TLS, TCP, and ping frames.
    void CountReceivedBytes(std::size_t a_bytes);
    void CountSentBytes(std::size_t a_bytes);

    [[nodiscard]] std::optional<TrafficRates> GetTrafficRates();
}
