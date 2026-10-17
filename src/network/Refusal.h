#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Network
{
    enum class RefusalReason : std::uint8_t
    {
        kProtocolMismatch,
        kDisplayNameTaken,
        kDisplayNameInvalid,
        kSessionReplaced,
        kServerFull,
        kTooManyNewSessions,
        kAddressFull
    };

    struct Refusal
    {
        RefusalReason reason{ RefusalReason::kProtocolMismatch };
        // The server's protocol version for kProtocolMismatch; empty otherwise.
        std::string detail;
    };

    [[nodiscard]] constexpr std::string_view GetRefusalName(RefusalReason a_reason)
    {
        switch (a_reason)
        {
            case RefusalReason::kProtocolMismatch:
                return "protocol mismatch";
            case RefusalReason::kDisplayNameTaken:
                return "display name taken";
            case RefusalReason::kDisplayNameInvalid:
                return "display name invalid";
            case RefusalReason::kSessionReplaced:
                return "session replaced";
            case RefusalReason::kServerFull:
                return "server full";
            case RefusalReason::kTooManyNewSessions:
                return "too many new sessions";
            case RefusalReason::kAddressFull:
                return "too many players from address";
        }
        return "unknown";
    }
}
