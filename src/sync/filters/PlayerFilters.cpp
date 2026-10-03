#include "sync/filters/PlayerFilters.h"
#include "network/PlayerFilterStore.h"
#include "network/ServerClient.h"
#include "sync/chat/ChatSync.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <mutex>
#include <utility>

namespace PlayerFilters
{
    namespace
    {
        std::mutex s_lock;
        // The connection generation the entries were read for.
        std::uint64_t s_generation = (std::numeric_limits<std::uint64_t>::max)();
        std::string s_server;
        // The current server's, sorted by session.
        std::vector<Entry> s_entries;
        std::atomic_uint64_t s_changes{ 0 };

        Network::PlayerFilterStore::Entry ToStored(const Entry& a_entry)
        {
            return { .session = a_entry.session, .name = a_entry.name, .muted = a_entry.muted, .vanished = a_entry.vanished };
        }

        // A server changes only with a connection, so the entries are read
        // again when the generation moves. Requires s_lock.
        void RefreshLocked()
        {
            const auto generation = Network::GetConnectionGeneration();
            if (generation == s_generation)
            {
                return;
            }
            s_generation = generation;
            s_server = Network::GetServerAddress();
            std::vector<Network::PlayerFilterStore::Entry> stored;
            Network::PlayerFilterStore::Copy(s_server, stored);
            s_entries.clear();
            for (auto& entry : stored)
            {
                s_entries.push_back(Entry{
                    .session = entry.session, .name = std::move(entry.name), .muted = entry.muted, .vanished = entry.vanished });
            }
            std::ranges::sort(s_entries, {}, &Entry::session);
        }

        // Requires s_lock.
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

        // Sets one flag of a_player's entry, adding or removing the entry, and
        // saves it under a_server, the server it belongs to.
        Change Set(const Multiplayer::RosterEntry& a_player, bool Entry::* a_flag, bool a_value, std::string& a_server)
        {
            Entry saved;
            {
                const std::scoped_lock lock{ s_lock };
                RefreshLocked();
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
                if (!entry->muted && !entry->vanished)
                {
                    s_entries.erase(entry);
                }
                a_server = s_server;
                s_changes.fetch_add(1, std::memory_order_acq_rel);
            }
            if (!Network::PlayerFilterStore::Save(a_server, ToStored(saved)))
            {
                ChatSync::AddNotice("Could not save the list of muted and vanished players; the change lasts until "
                                    "the game exits.");
            }
            return Change::kChanged;
        }
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

    void SetMuted(const Multiplayer::RosterEntry& a_player, bool a_muted)
    {
        std::string server;
        if (a_player.session == 0 || Set(a_player, &Entry::muted, a_muted, server) == Change::kNone)
        {
            return;
        }
        logs::info("Player filters: {} session {} on {}", a_muted ? "muted" : "unmuted",
            static_cast<unsigned long long>(a_player.session), server);
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
                ChatSync::AddNotice(std::format(
                    "You can vanish at most {} players on a server.", Multiplayer::kMaximumVanishedSessions));
                return;
            case Change::kChanged:
                break;
        }
        // After the store, which a hello not sent yet reads instead.
        Network::PublishVanish(server, a_player.session, a_vanished);
        logs::info("Player filters: {} session {} on {}", a_vanished ? "vanished" : "brought back",
            static_cast<unsigned long long>(a_player.session), server);
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
}
