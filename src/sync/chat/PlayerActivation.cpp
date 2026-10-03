#include "sync/chat/PlayerActivation.h"
#include "sync/chat/ChatRoster.h"

#include <mutex>
#include <utility>

namespace PlayerActivation
{
    namespace
    {
        std::mutex s_lock;
        std::optional<Multiplayer::RosterEntry> s_activated;
        std::optional<Multiplayer::RosterEntry> s_whisper;

        std::optional<Multiplayer::RosterEntry> Take(std::optional<Multiplayer::RosterEntry>& a_request)
        {
            const std::scoped_lock lock{ s_lock };
            return std::exchange(a_request, std::nullopt);
        }
    }

    void Activate(Multiplayer::PeerSlot a_playerID)
    {
        auto player = ChatRoster::Find(a_playerID);
        logs::debug("Local player activated a remote player: player={} listed={}", a_playerID, player.has_value());
        if (!player)
        {
            return;
        }
        const std::scoped_lock lock{ s_lock };
        s_activated = std::move(player);
    }

    std::optional<Multiplayer::RosterEntry> TakeActivated()
    {
        return Take(s_activated);
    }

    void RequestWhisper(const Multiplayer::RosterEntry& a_player)
    {
        const std::scoped_lock lock{ s_lock };
        s_whisper = a_player;
    }

    std::optional<Multiplayer::RosterEntry> TakeWhisperRequest()
    {
        return Take(s_whisper);
    }
}
