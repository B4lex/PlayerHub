#pragma once

#include <cstdint>
#include <string>

namespace Network
{
    // Why the server closed a connection in a way that retrying cannot fix.
    enum class RefusalReason : std::uint8_t
    {
        kProtocolMismatch,
        // Another session holds the display name the hello asked for.
        kDisplayNameTaken,
        kDisplayNameInvalid,
        // The session connected again from somewhere else, which closed this
        // connection. Retrying would close that one in turn.
        kSessionReplaced
    };

    struct Refusal
    {
        RefusalReason reason{ RefusalReason::kProtocolMismatch };
        // For the player, as a sentence.
        std::string message;
    };
}
