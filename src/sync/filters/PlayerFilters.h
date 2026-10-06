#pragma once

#include "protocol/ChatMessage.h"
#include "protocol/Session.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PlayerFilters
{
    struct Entry
    {
        Multiplayer::SessionID session{ 0 };
        std::string name;
        bool muted{ false };
        bool vanished{ false };
    };

    [[nodiscard]] bool IsMuted(Multiplayer::SessionID a_session);
    [[nodiscard]] bool IsVanished(Multiplayer::SessionID a_session);

    void SetMuted(const Multiplayer::RosterEntry& a_player, bool a_muted);
    void SetVanished(const Multiplayer::RosterEntry& a_player, bool a_vanished);

    [[nodiscard]] std::uint64_t GetRevision();

    void Copy(std::vector<Entry>& a_entries);
}
