#pragma once

#include <chrono>
#include <optional>

namespace Network
{
    void UpdateLatencyProbe(float a_deltaSeconds);
    [[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();
    [[nodiscard]] std::optional<double> GetServerTimeMilliseconds();
}
