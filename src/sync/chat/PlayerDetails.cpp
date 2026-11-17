#include "sync/chat/PlayerDetails.h"
#include "forms/FormLookup.h"
#include "network/ServerClient.h"
#include "runtime/GameThread.h"
#include "sync/PeerState.h"
#include "sync/trade/TradeSync.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace PlayerDetails
{
    namespace
    {
        std::mutex s_lock;
        Multiplayer::PeerSlot s_requested = 0;
        Multiplayer::SessionID s_requestedSession = 0;
        bool s_untaken = false;
        std::optional<Details> s_details;
        std::unordered_map<Multiplayer::SessionID, Details> s_known;

        bool IsInTradeRange(const PeerState& a_peer)
        {
            const auto actor = a_peer.lifecycle.actor ? a_peer.lifecycle.actor->handle.get() : nullptr;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto* cell = player ? player->GetParentCell() : nullptr;
            // Exterior cells meet at borders, while an interior's coordinates are its own.
            if (!actor || !cell || (cell->IsInteriorCell() && actor->GetParentCell() != cell))
            {
                return false;
            }
            // Horizontal, as the server measures.
            const auto offset = actor->GetPosition() - player->GetPosition();
            return offset.x * offset.x + offset.y * offset.y <= TradeSync::kRequestRadius * TradeSync::kRequestRadius;
        }

        template <class T>
        const char* GetFormName(const Multiplayer::FormReference& a_reference)
        {
            const auto* form = a_reference.IsEmpty() ? nullptr : FormLookup::ResolveForm<T>(a_reference);
            const char* name = form ? form->GetFullName() : nullptr;
            return name && *name != '\0' ? name : nullptr;
        }

        // Most exterior cells have no name, so a worldspace is named by the location within it.
        std::optional<std::string> GetPlaceName(const Multiplayer::FormReference& a_interiorCell,
            const Multiplayer::FormReference& a_worldspace, const Multiplayer::FormReference& a_location)
        {
            if (a_interiorCell.IsEmpty() && a_worldspace.IsEmpty() && a_location.IsEmpty())
            {
                return std::nullopt;
            }
            const char* name = GetFormName<RE::TESObjectCELL>(a_interiorCell);
            name = name ? name : GetFormName<RE::BGSLocation>(a_location);
            name = name ? name : GetFormName<RE::TESWorldSpace>(a_worldspace);
            return std::string{ name ? name : "" };
        }

        void Publish(Multiplayer::PeerSlot a_playerID, Details&& a_details)
        {
            const std::scoped_lock lock{ s_lock };
            if (a_playerID != s_requested)
            {
                return;
            }
            if (s_requestedSession != 0 && (a_details.level != 0 || a_details.place))
            {
                auto& known = s_known[s_requestedSession];
                known = a_details;
                known.inTradeRange = false;
            }
            s_details = std::move(a_details);
        }
    }

    void Request(Multiplayer::PeerSlot a_playerID, Multiplayer::SessionID a_session)
    {
        const std::scoped_lock lock{ s_lock };
        s_requested = a_playerID;
        s_requestedSession = a_session;
        s_untaken = true;
        s_details.reset();
    }

    std::optional<Details> Find(Multiplayer::PeerSlot a_playerID)
    {
        const std::scoped_lock lock{ s_lock };
        return a_playerID == s_requested ? s_details : std::nullopt;
    }

    std::optional<Details> FindKnown(Multiplayer::SessionID a_session)
    {
        const std::scoped_lock lock{ s_lock };
        const auto known = a_session != 0 ? s_known.find(a_session) : s_known.end();
        return known != s_known.end() ? std::optional{ known->second } : std::nullopt;
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
            const auto* newest = a_peer->playout.GetNewest();
            const auto& checkpoint = newest ? *newest : a_peer->playout.GetPlayed();
            logs::debug("Player details from the remote actor: player={} level={} location={}|{:06X}", a_playerID,
                appearance.level, checkpoint.location.plugin, checkpoint.location.localFormID);
            Publish(a_playerID,
                Details{ .level = appearance.level,
                    .place = GetPlaceName(checkpoint.interiorCell, checkpoint.worldspace, checkpoint.location),
                    .inTradeRange = IsInTradeRange(*a_peer) });
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
            logs::debug("Player details from the server: player={} level={} location={}|{:06X}", inspection.playerID,
                inspection.level, inspection.location.plugin, inspection.location.localFormID);
            Publish(inspection.playerID,
                Details{ .level = inspection.level,
                    .place = GetPlaceName(inspection.interiorCell, inspection.worldspace, inspection.location) });
        }
    }
}
