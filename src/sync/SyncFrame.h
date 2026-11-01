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
    float sampledSpeed{ 0.0F };
    float direction{ 0.0F };
    // Progress from the played checkpoint to the next one; 0 without a segment.
    float fraction{ 0.0F };
};

struct RemoteFrame
{
    Multiplayer::PeerSlot playerID;
    const Network::PlayerTransform& checkpoint;
    // The checkpoint the pose moves toward, if one waits.
    const Network::PlayerTransform* next;
    const RemotePose& pose;
    // Local FormIDs of the checkpoint's references; zero when not loaded here.
    RE::FormID cellID;
    RE::FormID worldspaceID;
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
