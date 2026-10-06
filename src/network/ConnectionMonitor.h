#pragma once

#include "network/Refusal.h"

#include <chrono>
#include <optional>

namespace Network
{
    enum class ConnectionState
    {
        kDisconnected,
        kDisconnecting,
        kConnecting,
        kConnected,
    };

    void UpdateConnectionMonitor(float a_deltaSeconds);

    void SetConnectionEnabled(bool a_enabled);

    [[nodiscard]] ConnectionState GetConnectionState();

    [[nodiscard]] std::optional<Refusal> GetConnectionRefusal();

    [[nodiscard]] std::optional<std::chrono::milliseconds> GetLatency();
}
