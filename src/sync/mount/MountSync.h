#pragma once

#include "actor/ActorMount.h"
#include "actor/RemoteActor.h"
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
        std::uint64_t appliedSequence{ 0 };
        float seatRetryRemaining{ 0.0F };
        float seatCheckRemaining{ 0.0F };
        std::uint32_t seatAttempts{ 0 };
        bool seated{ false };
        bool carriesRider{ false };
    };

    [[nodiscard]] RE::NiPointer<RE::Actor> SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence);

    [[nodiscard]] bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    [[nodiscard]] bool UpdateRider(PeerState& a_peer, float a_delta);
}
