#pragma once

#include "protocol/ChatMessage.h"

#include <optional>

namespace PlayerActivation
{
    void Activate(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] std::optional<Multiplayer::RosterEntry> TakeActivated();

    void RequestWhisper(const Multiplayer::RosterEntry& a_player);
    [[nodiscard]] std::optional<Multiplayer::RosterEntry> TakeWhisperRequest();
}
