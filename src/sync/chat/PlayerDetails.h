#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/PeerSlot.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct PeerState;

namespace PlayerDetails
{
    struct Details
    {
        std::string race;
        std::uint16_t level{ 0 };
    };

    void Request(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] std::optional<Details> Find(Multiplayer::PeerSlot a_playerID);

    [[nodiscard]] std::optional<Multiplayer::PeerSlot> TakeRequest();
    void Serve(Multiplayer::PeerSlot a_playerID, const PeerState* a_peer);
    void Accept(std::span<const Multiplayer::PlayerInspection> a_inspections);
}
