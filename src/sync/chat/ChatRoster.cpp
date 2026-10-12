#include "sync/chat/ChatRoster.h"
#include "network/ServerClient.h"
#include "protocol/AppearanceState.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace ChatRoster
{
    namespace
    {
        constexpr std::size_t kMaximumPartners = 8;

        using Entry = ListedPlayer;

        std::mutex s_lock;
        std::uint64_t s_revision = 0;
        Multiplayer::Roster s_roster;
        std::vector<Entry> s_entries;
        Multiplayer::SessionID s_localSession = 0;
        std::deque<Player> s_partners;
        std::optional<Player> s_lastWhisperer;
        std::vector<Multiplayer::PeerSlot> s_nearby;

        char32_t FoldCodePoint(char32_t a_code)
        {
            const auto pairs = [a_code](bool a_upperIsOdd) {
                return ((a_code & 1) != 0) == a_upperIsOdd ? a_code + 1 : a_code;
            };
            if (a_code < 0x80)
            {
                return a_code >= U'A' && a_code <= U'Z' ? a_code + 0x20 : a_code;
            }
            if (a_code >= 0xC0 && a_code <= 0xDE)
            {
                return a_code == 0xD7 ? a_code : a_code + 0x20;
            }
            if (a_code >= 0x100 && a_code <= 0x17F)
            {
                if (a_code == 0x130 || a_code == 0x131 || a_code == 0x138 || a_code == 0x149 || a_code == 0x17F)
                {
                    return a_code;
                }
                if (a_code == 0x178)
                {
                    return 0xFF;
                }
                return pairs((a_code >= 0x139 && a_code <= 0x148) || a_code >= 0x179);
            }
            if (a_code >= 0x386 && a_code <= 0x3AB)
            {
                if (a_code >= 0x391)
                {
                    return a_code == 0x3A2 ? a_code : a_code + 0x20;
                }
                switch (a_code)
                {
                    case 0x386:
                        return 0x3AC;
                    case 0x388:
                    case 0x389:
                    case 0x38A:
                        return a_code + 0x25;
                    case 0x38C:
                        return 0x3CC;
                    case 0x38E:
                    case 0x38F:
                        return a_code + 0x3F;
                    default:
                        return a_code;
                }
            }
            if (a_code == 0x3C2)
            {
                return 0x3C3;
            }
            if (a_code >= 0x400 && a_code <= 0x40F)
            {
                return a_code + 0x50;
            }
            if (a_code >= 0x410 && a_code <= 0x42F)
            {
                return a_code + 0x20;
            }
            if ((a_code >= 0x460 && a_code <= 0x481) || (a_code >= 0x48A && a_code <= 0x4BF)
                || (a_code >= 0x4D0 && a_code <= 0x52F))
            {
                return pairs(false);
            }
            if (a_code == 0x4C0)
            {
                return 0x4CF;
            }
            if (a_code >= 0x4C1 && a_code <= 0x4CE)
            {
                return pairs(true);
            }
            return a_code;
        }

        std::string Fold(std::string_view a_text)
        {
            std::string folded;
            folded.reserve(a_text.size());
            for (std::size_t i = 0; i < a_text.size(); ++i)
            {
                const auto lead = static_cast<unsigned char>(a_text[i]);
                if (lead < 0x80)
                {
                    folded.push_back(static_cast<char>(FoldCodePoint(lead)));
                    continue;
                }
                const auto next = i + 1 < a_text.size() ? static_cast<unsigned char>(a_text[i + 1]) : 0;
                if ((lead & 0xE0) == 0xC0 && (next & 0xC0) == 0x80)
                {
                    const auto code = FoldCodePoint(static_cast<char32_t>(((lead & 0x1F) << 6) | (next & 0x3F)));
                    folded.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    folded.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    ++i;
                    continue;
                }
                folded.push_back(a_text[i]);
            }
            return folded;
        }

        void RefreshLocked()
        {
            if (!Network::CopyRosterIfChanged(s_revision, s_roster))
            {
                return;
            }
            s_entries.clear();
            s_entries.reserve(s_roster.players.size());
            s_localSession = 0;
            for (const auto& player : s_roster.players)
            {
                s_entries.push_back(Entry{ .player = player, .folded = Fold(player.name) });
                if (player.playerID == s_roster.localPlayerID)
                {
                    s_localSession = player.session;
                }
            }
            std::ranges::sort(s_entries, {}, &Entry::folded);
        }

        const Entry* FindByNameLocked(std::string_view a_name)
        {
            const auto folded = Fold(a_name);
            const auto entry = std::ranges::lower_bound(s_entries, folded, {}, &Entry::folded);
            return entry != s_entries.end() && entry->folded == folded ? &*entry : nullptr;
        }
    }

    std::optional<Addressee> FindAddressee(std::string_view a_text)
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        for (auto end = a_text.size(); end != std::string_view::npos && end > 0; end = a_text.rfind(' ', end - 1))
        {
            if (end > Multiplayer::PlayerAppearance::kMaximumNameLength)
            {
                continue;
            }
            if (const auto* entry = FindByNameLocked(a_text.substr(0, end)))
            {
                return Addressee{ .player = entry->player,
                    .nameLength = end,
                    .text = Multiplayer::TrimChatText(a_text.substr(end)) };
            }
        }
        return std::nullopt;
    }

    std::optional<Player> Resolve(const Player& a_player)
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        const auto known = std::ranges::find(s_entries, a_player.playerID, [](const Entry& a_entry) {
            return a_entry.player.playerID;
        });
        if (known != s_entries.end())
        {
            return known->player;
        }
        if (const auto* entry = FindByNameLocked(a_player.name))
        {
            return entry->player;
        }
        return std::nullopt;
    }

    std::optional<Player> Find(Multiplayer::PeerSlot a_playerID)
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        const auto known = std::ranges::find(s_entries, a_playerID, [](const Entry& a_entry) {
            return a_entry.player.playerID;
        });
        return known != s_entries.end() ? std::optional{ known->player } : std::nullopt;
    }

    void Complete(std::string_view a_prefix, std::vector<Player>& a_players, std::size_t a_maximum)
    {
        const auto prefix = Fold(a_prefix);

        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        s_nearby.clear();
        CopyNearbyPlayers(s_nearby);

        const auto first = a_players.size();
        const auto full = [&] { return a_players.size() - first >= a_maximum; };
        const auto offer = [&](const Entry& a_entry) {
            if (a_entry.player.playerID != s_roster.localPlayerID && a_entry.folded.starts_with(prefix)
                && std::ranges::find(a_players.begin() + static_cast<std::ptrdiff_t>(first), a_players.end(),
                       a_entry.player.playerID, &Player::playerID)
                    == a_players.end())
            {
                a_players.push_back(a_entry.player);
            }
        };

        if (const auto* exact = FindByNameLocked(a_prefix); exact && !full())
        {
            offer(*exact);
        }
        for (const auto& partner : s_partners)
        {
            if (full())
            {
                return;
            }
            const auto entry = std::ranges::find(s_entries, partner.playerID, [](const Entry& a_entry) {
                return a_entry.player.playerID;
            });
            if (entry != s_entries.end())
            {
                offer(*entry);
            }
        }

        const auto begin = std::ranges::lower_bound(s_entries, prefix, {}, &Entry::folded);
        for (const bool nearby : { true, false })
        {
            for (auto entry = begin; entry != s_entries.end() && entry->folded.starts_with(prefix); ++entry)
            {
                if (full())
                {
                    return;
                }
                if (std::ranges::binary_search(s_nearby, entry->player.playerID) == nearby)
                {
                    offer(*entry);
                }
            }
        }
    }

    std::size_t GetPlayerCount()
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        return s_entries.size();
    }

    std::uint64_t GetRevision()
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        return s_revision;
    }

    Multiplayer::SessionID GetLocalSession()
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        return s_localSession;
    }

    bool CopyPlayersIfChanged(
        std::uint64_t& a_revision, std::vector<ListedPlayer>& a_players, Multiplayer::PeerSlot& a_localPlayer)
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        if (a_revision == s_revision)
        {
            return false;
        }
        a_revision = s_revision;
        a_players = s_entries;
        a_localPlayer = s_roster.localPlayerID;
        return true;
    }

    std::string FoldName(std::string_view a_name)
    {
        return Fold(a_name);
    }

    void CopyNearbyPlayers(std::vector<Multiplayer::PeerSlot>& a_players)
    {
        Network::CopyNearbyPlayers(a_players);
    }

    void NoteWhisper(const Player& a_player, bool a_incoming)
    {
        const std::scoped_lock lock{ s_lock };
        const auto folded = Fold(a_player.name);
        std::erase_if(s_partners, [&](const Player& a_partner) {
            return a_partner.playerID == a_player.playerID || Fold(a_partner.name) == folded;
        });
        s_partners.push_front(a_player);
        if (s_partners.size() > kMaximumPartners)
        {
            s_partners.pop_back();
        }
        if (a_incoming)
        {
            s_lastWhisperer = a_player;
        }
    }

    void GetRecentPartners(std::vector<Player>& a_players, std::size_t a_maximum)
    {
        const std::scoped_lock lock{ s_lock };
        for (std::size_t i = 0; i < s_partners.size() && i < a_maximum; ++i)
        {
            a_players.push_back(s_partners[i]);
        }
    }

    std::optional<Player> GetLastWhisperer()
    {
        const std::scoped_lock lock{ s_lock };
        return s_lastWhisperer;
    }
}
