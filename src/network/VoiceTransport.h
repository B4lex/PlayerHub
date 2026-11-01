#pragma once

#include "protocol/VoiceFrame.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Network::VoiceTransport
{
    enum class Route : std::uint8_t
    {
        // The connection has no voice.
        kNone,
        // Datagrams are not confirmed yet, or stopped arriving.
        kWebSocket,
        kDatagrams
    };

    // Called on the transport's threads for each frame received, by datagram or WebSocket.
    using Sink = void (*)(const Multiplayer::VoiceFrame&);

    void SetSink(Sink a_sink);

    // ServerClient's side: a welcome with voice access starts a route, and a lost connection ends it.
    void Start(const Multiplayer::VoiceAccess& a_access, std::string_view a_webSocketUrl);
    void Stop();
    void DeliverFromWebSocket(const Multiplayer::VoiceFrame& a_frame);

    // Any thread. Dropped without a route.
    void Send(std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus);

    [[nodiscard]] Route GetRoute();

    // The relay's voice radius in game units, or 0 without a route.
    [[nodiscard]] std::uint32_t GetRadius();
}
