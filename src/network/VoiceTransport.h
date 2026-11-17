#pragma once

#include "protocol/VoiceFrame.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Network::VoiceTransport
{
    enum class State : std::uint8_t
    {
        // The connection has no voice.
        kOff,
        // Probing the relay, which has not answered yet.
        kConnecting,
        kConnected,
        // The relay has not answered for a while, or the socket could not open.
        kUnreachable
    };

    // Called on the transport's thread for each frame received.
    using Sink = void (*)(const Multiplayer::VoiceFrame&);

    void SetSink(Sink a_sink);

    // ServerClient's side: a welcome with voice access starts the transport, and a lost connection
    // ends it.
    void Start(const Multiplayer::VoiceAccess& a_access, std::string_view a_serverHost);
    void Stop();

    // Any thread. Dropped unless kConnected.
    void Send(std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus);

    [[nodiscard]] State GetState();

    // The relay's voice radius in game units, or 0 while kOff.
    [[nodiscard]] std::uint32_t GetRadius();
}
