#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerInspection.h"
#include "protocol/Session.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct PeerState;

namespace PlayerDetails
{
    struct Details
    {
        std::uint16_t level{ 0 };
        // Where the player is; empty when the place is not in the local load order, unset when
        // the player is out of the world.
        std::optional<std::string> place;
        // Whether the player's character is near enough in the local world to trade.
        bool inTradeRange{ false };
        bool answered{ false };
    };

    void Request(Multiplayer::PeerSlot a_playerID, Multiplayer::SessionID a_session);
    [[nodiscard]] std::optional<Details> Find(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] std::optional<Details> FindKnown(Multiplayer::SessionID a_session);

    [[nodiscard]] std::optional<Multiplayer::PeerSlot> TakeRequest();
    void Serve(Multiplayer::PeerSlot a_playerID, const PeerState* a_peer);
    void Accept(std::span<const Multiplayer::PlayerInspection> a_inspections);
}
