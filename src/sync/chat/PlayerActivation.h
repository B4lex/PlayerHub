#pragma once

#include "protocol/ChatMessage.h"

#include <optional>

namespace PlayerActivation
{
    void Activate(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] std::optional<Multiplayer::PlayerIdentity> TakeActivated();

    void RequestWhisper(const Multiplayer::PlayerIdentity& a_player);
    [[nodiscard]] std::optional<Multiplayer::PlayerIdentity> TakeWhisperRequest();
}
