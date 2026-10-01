#pragma once

#include <cstdint>
#include <string>

namespace Network
{
    // Why the server closed a connection in a way that retrying cannot fix.
    enum class RefusalReason : std::uint8_t
    {
        kProtocolMismatch,
        // Another session holds the nickname the hello asked for.
        kNicknameTaken,
        kNicknameInvalid,
        // The session connected again from somewhere else, which closed this
        // connection. Retrying would close that one in turn.
        kSessionReplaced
    };

    struct Refusal
    {
        RefusalReason reason{ RefusalReason::kProtocolMismatch };
        // For the player, as a sentence.
        std::string message;
        // The nickname the refused attempt asked for.
        std::string nickname;
    };
}
