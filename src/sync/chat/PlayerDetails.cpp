#include "sync/chat/PlayerDetails.h"
#include "network/ServerClient.h"
#include "runtime/GameThread.h"
#include "sync/PeerState.h"
#include "sync/appearance/AppearanceSync.h"

#include <mutex>
#include <utility>

namespace PlayerDetails
{
    namespace
    {
        std::mutex s_lock;
        Multiplayer::PeerSlot s_requested = 0;
        bool s_untaken = false;
        std::optional<Details> s_details;

        void Publish(Multiplayer::PeerSlot a_playerID, Details&& a_details)
        {
            const std::scoped_lock lock{ s_lock };
            if (a_playerID == s_requested)
            {
                s_details = std::move(a_details);
            }
        }
    }

    void Request(Multiplayer::PeerSlot a_playerID)
    {
        const std::scoped_lock lock{ s_lock };
        s_requested = a_playerID;
        s_untaken = true;
        s_details.reset();
    }

    std::optional<Details> Find(Multiplayer::PeerSlot a_playerID)
    {
        const std::scoped_lock lock{ s_lock };
        return a_playerID == s_requested ? s_details : std::nullopt;
    }

    std::optional<Multiplayer::PeerSlot> TakeRequest()
    {
        const std::scoped_lock lock{ s_lock };
        if (!std::exchange(s_untaken, false))
        {
            return std::nullopt;
        }
        return s_requested;
    }

    void Serve(Multiplayer::PeerSlot a_playerID, const PeerState* a_peer)
    {
        if (!GameThread::Require("PlayerDetails::Serve"))
        {
            return;
        }
        if (a_peer && a_peer->lifecycle.actor && a_peer->appearance.latest)
        {
            const auto& appearance = *a_peer->appearance.latest;
            logs::debug("Player details from the remote actor: player={} level={}", a_playerID, appearance.level);
            Publish(a_playerID,
                Details{ .race = AppearanceSync::GetRaceName(a_playerID, appearance.race, appearance.defaultRace),
                    .level = appearance.level });
            return;
        }
        const bool requested = Network::RequestInspection(a_playerID);
        logs::debug("Player details requested from the server: player={} sent={}", a_playerID, requested);
        if (!requested)
        {
            Publish(a_playerID, {});
        }
    }

    void Accept(std::span<const Multiplayer::PlayerInspection> a_inspections)
    {
        if (a_inspections.empty() || !GameThread::Require("PlayerDetails::Accept"))
        {
            return;
        }
        for (const auto& inspection : a_inspections)
        {
            logs::debug("Player details from the server: player={} level={} race={}", inspection.playerID,
                inspection.level, inspection.race ? inspection.race->plugin : std::string{ "none" });
            Publish(inspection.playerID,
                Details{ .race = inspection.race
                        ? AppearanceSync::GetRaceName(inspection.playerID, *inspection.race, inspection.defaultRace)
                        : std::string{},
                    .level = inspection.level });
        }
    }
}
