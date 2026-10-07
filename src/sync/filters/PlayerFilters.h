#pragma once

#include "network/PlayerFilterStore.h"
#include "protocol/ChatMessage.h"
#include "protocol/Session.h"

#include <cstdint>
#include <vector>

namespace PlayerFilters
{
    using Entry = Network::PlayerFilterStore::Entry;

    [[nodiscard]] bool IsFriend(Multiplayer::SessionID a_session);
    [[nodiscard]] bool IsMuted(Multiplayer::SessionID a_session);
    [[nodiscard]] bool IsVanished(Multiplayer::SessionID a_session);

    void SetFriend(const Multiplayer::RosterEntry& a_player, bool a_friend);
    void SetMuted(const Multiplayer::RosterEntry& a_player, bool a_muted);
    void SetVanished(const Multiplayer::RosterEntry& a_player, bool a_vanished);

    [[nodiscard]] std::uint64_t GetRevision();

    void Copy(std::vector<Entry>& a_entries);

    void CollectArrivedFriends(std::vector<Multiplayer::RosterEntry>& a_friends);
}
