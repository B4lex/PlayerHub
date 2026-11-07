#pragma once

#include "actor/ActorMount.h"
#include "actor/ActorFlight.h"
#include "actor/RemoteActor.h"
#include "protocol/FlyState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct PeerState;

namespace MountSync
{
    struct RemoteMount
    {
        RemoteActor actor;
        ActorMount::Gait gait;
        ActorMount::Gait riderGait;
        ActorFlight::Flight flight;
        std::uint64_t appliedSequence{ 0 };
        float seatRetryRemaining{ 0.0F };
        float seatCheckRemaining{ 0.0F };
        std::uint32_t seatAttempts{ 0 };
        bool seated{ false };
        bool carriesRider{ false };
        bool flying{ false };
    };

    struct LocalMount
    {
        RE::NiPointer<RE::Actor> actor;
        bool flying{ false };
        Multiplayer::FlyState flyState{ Multiplayer::FlyState::kNone };
        Multiplayer::WingState wingState{ Multiplayer::WingState::kNone };
    };

    [[nodiscard]] LocalMount SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence, float a_elapsed);

    [[nodiscard]] bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    [[nodiscard]] bool UpdateRider(
        const RemoteFrame& a_remote, PeerState& a_peer, bool a_readyForWeaponDraw, float a_delta);
}
