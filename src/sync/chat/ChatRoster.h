#pragma once

#include "protocol/ChatMessage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ChatRoster
{
    using Player = Multiplayer::PlayerIdentity;

    struct ListedPlayer
    {
        Player player;
        std::string folded;
    };

    struct Addressee
    {
        Player player;
        std::size_t nameLength{ 0 };
        std::string_view text;
    };

    [[nodiscard]] std::optional<Addressee> FindAddressee(std::string_view a_text);

    [[nodiscard]] std::optional<Player> Resolve(const Player& a_player);

    [[nodiscard]] std::optional<Player> Find(Multiplayer::PeerSlot a_playerID);

    void Complete(std::string_view a_prefix, std::vector<Player>& a_players, std::size_t a_maximum);

    [[nodiscard]] std::size_t GetPlayerCount();

    [[nodiscard]] std::uint64_t GetRevision();

    [[nodiscard]] Multiplayer::SessionID GetLocalSession();

    [[nodiscard]] bool CopyPlayersIfChanged(
        std::uint64_t& a_revision, std::vector<ListedPlayer>& a_players, Multiplayer::PeerSlot& a_localPlayer);

    [[nodiscard]] std::string FoldName(std::string_view a_name);

    void CopyNearbyPlayers(std::vector<Multiplayer::PeerSlot>& a_players);

    void NoteWhisper(const Player& a_player);

    void GetRecentPartners(std::vector<Player>& a_players, std::size_t a_maximum);
}
