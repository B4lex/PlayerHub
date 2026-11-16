#pragma once

#include "protocol/ChatMessage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ChatRoster
{
    using Player = Multiplayer::PlayerIdentity;

    // What stands for a role beside its holder's name.
    struct RoleMark
    {
        // The role name's first character in upper case as UTF-8 text; empty without a role.
        std::array<char, 5> letter{};
        // 0xRRGGBB
        std::uint32_t color{ 0 };

        [[nodiscard]] explicit operator bool() const { return letter[0] != '\0'; }
    };

    struct ListedPlayer
    {
        Player player;
        std::string folded;
        RoleMark role;
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

    // Of the server's current roles, so a role deleted since has neither.
    [[nodiscard]] std::optional<Multiplayer::Role> FindRole(Multiplayer::RoleID a_role);
    [[nodiscard]] RoleMark FindRoleMark(Multiplayer::RoleID a_role);

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
