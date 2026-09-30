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

// A peer's transform at its playback time, between the played checkpoint and
// the next one waiting to play.
struct RemotePose
{
    RE::NiPoint3 position{};
    float heading{ 0.0F };
    float pitch{ 0.0F };
    // Speed along the interpolated path, or the played checkpoint's speed when
    // there is nothing to interpolate toward.
    float speed{ 0.0F };
};

struct RemoteFrame
{
    Multiplayer::PeerSlot playerID;
    // The played checkpoint and its sequence. Newer checkpoints may have
    // arrived and wait for their playback time.
    const Network::PlayerTransform& checkpoint;
    std::uint64_t lastSequence;
    const RemotePose& pose;
    // The checkpoint's furniture in the local load order: zero for none, or when
    // its plugin is not loaded here.
    RE::FormID furnitureRefID;
};

// A checkpoint reaching its playback time, before it replaces the previously
// played one. inserted marks the first one played since the player entered.
struct CheckpointUpdate
{
    Multiplayer::PeerSlot playerID;
    bool inserted;
    const Network::PlayerTransform& previous;
    std::uint64_t previousSequence;
    const Network::PlayerTransform& checkpoint;
};
