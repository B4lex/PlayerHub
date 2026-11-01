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
    // Percent, Network::PlayerFilterStore::kDefaultVoiceVolume for a player without an entry.
    [[nodiscard]] std::uint32_t GetVoiceVolume(Multiplayer::SessionID a_session);

    void SetFriend(const Multiplayer::PlayerIdentity& a_player, bool a_friend);
    void SetMuted(const Multiplayer::PlayerIdentity& a_player, bool a_muted);
    void SetVanished(const Multiplayer::PlayerIdentity& a_player, bool a_vanished);
    void SetVoiceVolume(const Multiplayer::PlayerIdentity& a_player, std::uint32_t a_percent);

    [[nodiscard]] std::uint64_t GetRevision();

    void Copy(std::vector<Entry>& a_entries);

    void CollectArrivedFriends(std::vector<Multiplayer::PlayerIdentity>& a_friends);
}
