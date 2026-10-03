#pragma once

#include "protocol/ChatMessage.h"
#include "protocol/Session.h"

#include <cstdint>
#include <string>
#include <vector>

// The players the local player muted or vanished on the current server
// (Network::GetServerAddress), by session, kept across games. Muting hides a
// player's chat lines on this client; vanishing has the server stop sending
// the player's presentation, so its character leaves this world. Neither is
// told to the player, and neither is mutual. Safe to call from any thread.
namespace PlayerFilters
{
    struct Entry
    {
        Multiplayer::SessionID session{ 0 };
        // As the player was named when last muted or vanished.
        std::string name;
        bool muted{ false };
        bool vanished{ false };
    };

    [[nodiscard]] bool IsMuted(Multiplayer::SessionID a_session);
    [[nodiscard]] bool IsVanished(Multiplayer::SessionID a_session);

    // A player without a session cannot be muted or vanished.
    void SetMuted(const Multiplayer::RosterEntry& a_player, bool a_muted);
    void SetVanished(const Multiplayer::RosterEntry& a_player, bool a_vanished);

    // Advances with every change, and whenever a connection may have changed
    // the server. Cheap enough to poll every frame.
    [[nodiscard]] std::uint64_t GetRevision();

    // Replaces a_entries with the current server's, in no particular order.
    void Copy(std::vector<Entry>& a_entries);
}
