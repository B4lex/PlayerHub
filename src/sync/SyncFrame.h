#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"

#include <RE/Skyrim.h>

#include <cstdint>

// Short-lived views for one main-loop synchronization pass. They borrow state
// owned by SyncCoordinator and must not outlive the pass. Cross-feature results
// are passed to each stage explicitly rather than through these views.

struct LocalFrame
{
    RE::PlayerCharacter& player;
};

struct RemoteFrame
{
    Multiplayer::PeerSlot playerID;
    // The newest accepted checkpoint and its sequence.
    const Network::PlayerTransform& checkpoint;
    std::uint64_t lastSequence;
    // The checkpoint's furniture in the local load order: zero for none, or when
    // its plugin is not loaded here.
    RE::FormID furnitureRefID;
};

// A newer checkpoint accepted from the network, before it replaces the previous
// one. inserted marks the first message ever received for the player.
struct CheckpointUpdate
{
    Multiplayer::PeerSlot playerID;
    bool inserted;
    const Network::PlayerTransform& previous;
    std::uint64_t previousSequence;
    const Network::PlayerTransform& checkpoint;
};
