#pragma once

#include "protocol/PlayerState.h"
#include "sync/ActorLifecycle.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/mount/MountSync.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// Complete receive state of one network player. Composition only: each feature
// owns and interprets its own member. Accessed only by the main-loop pass.
struct PeerState
{
    Network::PlayerTransform latestCheckpoint;
    std::uint64_t lastSequence{ 0 };
    // Checkpoint sequence whose transform the current actor last received.
    std::uint64_t appliedSequence{ 0 };
    CombatSync::RemoteState combat;
    MountSync::RemoteState mounting;
    ActorLifecycleState lifecycle;
    AppearanceSync::RemoteState appearance;
    EquipmentSync::RemoteState equipment;
};

using Peers = std::unordered_map<std::string, PeerState>;
