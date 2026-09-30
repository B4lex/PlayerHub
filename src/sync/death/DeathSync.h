#pragma once

#include "sync/SyncFrame.h"

#include <cstdint>

struct PeerState;

// A dying or dead player is presented by killing its remote actor into a local
// ragdoll. The body is then left to Skyrim, so where it comes to rest is local.
// Vanilla death ends in a load, whose world exit removes the body.
namespace DeathSync
{
    // Local stage, called by CheckpointPublisher once per published checkpoint.
    [[nodiscard]] bool SampleLocalDeath(const LocalFrame& a_local, std::uint64_t a_sequence);

    // Remote stage for a peer with an actor, before any stage that drives it.
    // Kills a ready actor whose player died, and retires a killed one whose
    // player is alive again so it respawns. Returns whether the actor is left to
    // its local death, in which case no other stage may drive it.
    [[nodiscard]] bool SyncDeath(const RemoteFrame& a_remote, PeerState& a_peer);
}
