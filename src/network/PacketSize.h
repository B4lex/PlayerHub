#pragma once

#include <sio_message.h>

#include <cstddef>
#include <string_view>

namespace Network
{
    // The two JSON writers differ only in strings: the server (Python's json)
    // escapes every non-ASCII character, while the client (rapidjson) writes
    // UTF-8 as is.
    enum class JsonWriter
    {
        kServer,
        kClient,
    };

    // Estimated Engine.IO message size of a Socket.IO event on the default
    // namespace, from the parsed or outgoing message. Exact apart from number
    // formatting quirks and the ack ID, which the client never exposes.
    [[nodiscard]] std::size_t EstimateEventPacketSize(
        std::string_view a_event, const sio::message::list& a_arguments, JsonWriter a_writer);

    // Estimated Engine.IO message size of a Socket.IO acknowledgement.
    [[nodiscard]] std::size_t EstimateAcknowledgementPacketSize(
        const sio::message::list& a_arguments, JsonWriter a_writer);
}
