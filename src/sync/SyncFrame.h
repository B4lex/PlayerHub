#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct LocalFrame
{
    RE::PlayerCharacter& player;
};

struct RemotePose
{
    RE::NiPoint3 position{};
    float heading{ 0.0F };
    float headingRate{ 0.0F };
    float pitch{ 0.0F };
    float speed{ 0.0F };
};

struct RemoteFrame
{
    Multiplayer::PeerSlot playerID;
    const Network::PlayerTransform& checkpoint;
    const RemotePose& pose;
    RE::FormID furnitureRefID;
};

struct CheckpointUpdate
{
    Multiplayer::PeerSlot playerID;
    bool inserted;
    const Network::PlayerTransform& previous;
    std::uint64_t previousSequence;
    const Network::PlayerTransform& checkpoint;
};
