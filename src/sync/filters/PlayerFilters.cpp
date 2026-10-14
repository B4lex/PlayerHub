#include "sync/filters/PlayerFilters.h"
#include "network/ServerClient.h"
#include "sync/notices/Notices.h"
#include "text/Text.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace PlayerFilters
{
    namespace
    {
        std::mutex s_lock;
        std::uint64_t s_generation = (std::numeric_limits<std::uint64_t>::max)();
        std::string s_server;
        std::vector<Entry> s_entries;
        std::atomic_uint64_t s_changes{ 0 };

        std::mutex s_rosterLock;
        std::uint64_t s_rosterRevision = 0;
        Multiplayer::Roster s_roster;
        Multiplayer::PeerSlot s_rosterLocalPlayer = 0;
        std::vector<Multiplayer::SessionID> s_online;
        std::vector<std::uint32_t> s_arrived;
        std::vector<Entry> s_renamed;

        void RefreshLocked()
        {
            const auto generation = Network::GetConnectionGeneration();
            if (generation == s_generation)
            {
                return;
            }
            s_generation = generation;
            s_server = Network::GetServerAddress();
            s_entries.clear();
            Network::PlayerFilterStore::Copy(s_server, s_entries);
            std::ranges::sort(s_entries, {}, &Entry::session);
        }

        const Entry* FindLocked(Multiplayer::SessionID a_session)
        {
            RefreshLocked();
            const auto entry = std::ranges::lower_bound(s_entries, a_session, {}, &Entry::session);
            return entry != s_entries.end() && entry->session == a_session ? &*entry : nullptr;
        }

        enum class Change
        {
            kNone,
            kChanged,
            kFull
        };

        void Save(const std::string& a_server, const Entry& a_entry)
        {
            if (!Network::PlayerFilterStore::Save(a_server, a_entry))
            {
                Notices::Post(std::string{ Text::Get(Text::Id::kPlayersSaveFailed) });
            }
        }

        Change Set(const Multiplayer::RosterEntry& a_player, bool Entry::* a_flag, bool a_value, std::string& a_server)
        {
            Entry saved;
            {
                const std::scoped_lock lock{ s_lock };
                RefreshLocked();
                if (s_server.empty())
                {
                    return Change::kNone;
                }
                auto entry = std::ranges::lower_bound(s_entries, a_player.session, {}, &Entry::session);
                const bool found = entry != s_entries.end() && entry->session == a_player.session;
                if ((found ? (*entry).*a_flag : false) == a_value)
                {
                    return Change::kNone;
                }
                if (a_flag == &Entry::vanished && a_value
                    && std::cmp_greater_equal(
                        std::ranges::count_if(s_entries, &Entry::vanished), Multiplayer::kMaximumVanishedSessions))
                {
                    return Change::kFull;
                }
                if (!found)
                {
                    entry = s_entries.insert(entry, Entry{ .session = a_player.session });
                }
                (*entry).*a_flag = a_value;
                entry->name = a_player.name;
                saved = *entry;
                if (!entry->friended && !entry->muted && !entry->vanished)
                {
                    s_entries.erase(entry);
                }
                a_server = s_server;
                s_changes.fetch_add(1, std::memory_order_acq_rel);
            }
            Save(a_server, saved);
            return Change::kChanged;
        }
    }

    bool IsFriend(Multiplayer::SessionID a_session)
    {
        const std::scoped_lock lock{ s_lock };
        const auto* entry = FindLocked(a_session);
        return entry && entry->friended;
    }

    bool IsMuted(Multiplayer::SessionID a_session)
    {
        const std::scoped_lock lock{ s_lock };
        const auto* entry = FindLocked(a_session);
        return entry && entry->muted;
    }

    bool IsVanished(Multiplayer::SessionID a_session)
    {
        const std::scoped_lock lock{ s_lock };
        const auto* entry = FindLocked(a_session);
        return entry && entry->vanished;
    }

    void SetFriend(const Multiplayer::RosterEntry& a_player, bool a_friend)
    {
        std::string server;
        if (a_player.session == 0 || Set(a_player, &Entry::friended, a_friend, server) == Change::kNone)
        {
            return;
        }
        logs::info("Player filters: {} session {} on {}", a_friend ? "befriended" : "unfriended",
            static_cast<unsigned long long>(a_player.session), Network::DescribeServer(server));
    }

    void SetMuted(const Multiplayer::RosterEntry& a_player, bool a_muted)
    {
        std::string server;
        if (a_player.session == 0 || Set(a_player, &Entry::muted, a_muted, server) == Change::kNone)
        {
            return;
        }
        logs::info("Player filters: {} session {} on {}", a_muted ? "muted" : "unmuted",
            static_cast<unsigned long long>(a_player.session), Network::DescribeServer(server));
    }

    void SetVanished(const Multiplayer::RosterEntry& a_player, bool a_vanished)
    {
        if (a_player.session == 0)
        {
            return;
        }
        std::string server;
        switch (Set(a_player, &Entry::vanished, a_vanished, server))
        {
            case Change::kNone:
                return;
            case Change::kFull:
                Notices::Post(Text::FormatPlural(Text::Id::kPlayersVanishLimit, Multiplayer::kMaximumVanishedSessions));
                return;
            case Change::kChanged:
                break;
        }
        // After the store, which a hello not sent yet reads instead.
        Network::PublishVanish(server, a_player.session, a_vanished);
        logs::info("Player filters: {} session {} on {}", a_vanished ? "vanished" : "brought back",
            static_cast<unsigned long long>(a_player.session), Network::DescribeServer(server));
    }

    std::uint64_t GetRevision()
    {
        // Both only rise, so their sum changes with either.
        return s_changes.load(std::memory_order_acquire) + Network::GetConnectionGeneration();
    }

    void Copy(std::vector<Entry>& a_entries)
    {
        const std::scoped_lock lock{ s_lock };
        RefreshLocked();
        a_entries = s_entries;
    }

    void CollectArrivedFriends(std::vector<Multiplayer::RosterEntry>& a_friends)
    {
        const std::scoped_lock rosterLock{ s_rosterLock };
        if (!Network::CopyRosterIfChanged(s_rosterRevision, s_roster))
        {
            return;
        }
        const auto& players = s_roster.players;
        if (std::ranges::find(players, s_roster.localPlayerID, &Multiplayer::RosterEntry::playerID) == players.end())
        {
            s_online.clear();
            s_rosterLocalPlayer = 0;
            return;
        }
        // A connection's first roster lists everyone already online; only later joins arrive.
        const bool first = s_roster.localPlayerID != s_rosterLocalPlayer;
        s_rosterLocalPlayer = s_roster.localPlayerID;
        s_arrived.clear();
        for (std::uint32_t index = 0; index < players.size(); ++index)
        {
            if (first || !std::ranges::binary_search(s_online, players[index].session))
            {
                s_arrived.push_back(index);
            }
        }
        s_online.clear();
        for (const auto& player : players)
        {
            s_online.push_back(player.session);
        }
        std::ranges::sort(s_online);
        if (s_arrived.empty())
        {
            return;
        }

        std::string server;
        {
            const std::scoped_lock lock{ s_lock };
            RefreshLocked();
            for (const auto index : s_arrived)
            {
                const auto& player = players[index];
                const auto entry = std::ranges::lower_bound(s_entries, player.session, {}, &Entry::session);
                if (entry == s_entries.end() || entry->session != player.session)
                {
                    continue;
                }
                if (entry->name != player.name)
                {
                    entry->name = player.name;
                    s_renamed.push_back(*entry);
                }
                if (!first && entry->friended)
                {
                    a_friends.push_back(player);
                }
            }
            if (!s_renamed.empty())
            {
                server = s_server;
                s_changes.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        for (const auto& entry : s_renamed)
        {
            Save(server, entry);
        }
        s_renamed.clear();
    }
}
