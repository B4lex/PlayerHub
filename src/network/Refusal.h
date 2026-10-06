#pragma once

#include <cstdint>
#include <string>

namespace Network
{
    enum class RefusalReason : std::uint8_t
    {
        kProtocolMismatch,
        kDisplayNameTaken,
        kDisplayNameInvalid,
        kSessionReplaced,
        kServerFull,
        kTooManyNewSessions
    };

    struct Refusal
    {
        RefusalReason reason{ RefusalReason::kProtocolMismatch };
        std::string message;
    };
}
