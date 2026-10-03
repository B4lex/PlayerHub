#pragma once

#include "protocol/ChatMessage.h"

#include <optional>

// The local player activating another player's character, which opens that
// player's menu in the world, and a whisper picked there, which opens the
// chat. Each request is taken once; a newer one replaces one not yet taken.
// Safe to call from any thread.
namespace PlayerActivation
{
    // Main-loop thread: the player whose character was activated. Nothing
    // for a player the roster does not list.
    void Activate(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] std::optional<Multiplayer::RosterEntry> TakeActivated();

    void RequestWhisper(const Multiplayer::RosterEntry& a_player);
    [[nodiscard]] std::optional<Multiplayer::RosterEntry> TakeWhisperRequest();
}
