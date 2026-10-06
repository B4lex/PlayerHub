#pragma once

#include "sync/SyncFrame.h"

#include <cstdint>

struct PeerState;

namespace DeathSync
{
    [[nodiscard]] bool SampleLocalDeath(const LocalFrame& a_local, std::uint64_t a_sequence);

    [[nodiscard]] bool SyncDeath(const RemoteFrame& a_remote, PeerState& a_peer);
}
