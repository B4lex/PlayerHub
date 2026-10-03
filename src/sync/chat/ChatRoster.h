#pragma once

#include "protocol/ChatMessage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The connected players a whisper can address, by name, and the ones the local
// player whispers with. Names compare without case for Latin, Greek, and
// Cyrillic letters. Safe to call from any thread.
namespace ChatRoster
{
    using Player = Multiplayer::RosterEntry;

    struct ListedPlayer
    {
        Player player;
        // As names are compared.
        std::string folded;
    };

    struct Addressee
    {
        Player player;
        // The bytes of the text that name the player.
        std::size_t nameLength{ 0 };
        // What follows the name, without surrounding spaces.
        std::string_view text;
    };

    // The player named at the start of a_text: the longest name followed by a
    // space or the end, since names may contain spaces.
    [[nodiscard]] std::optional<Addressee> FindAddressee(std::string_view a_text);

    // The connection a whisper to a_player reaches now: its own while it
    // lasts, otherwise the one that has its name, since a player who connects
    // again has another slot.
    [[nodiscard]] std::optional<Player> Resolve(const Player& a_player);

    // Appends at most a_maximum other players whose names start with a_prefix:
    // the player named exactly that, then recent whisper partners, then
    // players nearby, then the rest by name.
    void Complete(std::string_view a_prefix, std::vector<Player>& a_players, std::size_t a_maximum);

    // Connected players, the local one included; zero while disconnected.
    [[nodiscard]] std::size_t GetPlayerCount();

    // The local player's session as the roster lists it; zero while
    // disconnected.
    [[nodiscard]] Multiplayer::SessionID GetLocalSession();

    // Copies the connected players, sorted by folded name, and the local
    // player's slot when they changed since a_revision; the first call passes
    // zero.
    [[nodiscard]] bool CopyPlayersIfChanged(
        std::uint64_t& a_revision, std::vector<ListedPlayer>& a_players, Multiplayer::PeerSlot& a_localPlayer);

    // a_name as names are compared, for matching against ListedPlayer::folded.
    [[nodiscard]] std::string FoldName(std::string_view a_name);

    // The slots of the players near the local one, sorted.
    void CopyNearbyPlayers(std::vector<Multiplayer::PeerSlot>& a_players);

    // Records a whisper with a_player, received when a_incoming.
    void NoteWhisper(const Player& a_player, bool a_incoming);

    // Appends the latest whisper partners, most recent first.
    void GetRecentPartners(std::vector<Player>& a_players, std::size_t a_maximum);

    // The sender of the latest received whisper.
    [[nodiscard]] std::optional<Player> GetLastWhisperer();
}
