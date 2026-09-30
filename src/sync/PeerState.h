#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPlayout.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"

#include <cstdint>
#include <unordered_map>

// Complete receive state of one network player. Composition only: each feature
// owns and interprets its own member. Accessed only by the main-loop pass.
struct PeerState
{
    CheckpointPlayout playout;
    // Checkpoint sequence whose transform the current actor last received.
    std::uint64_t appliedSequence{ 0 };
    CombatSync::RemoteState combat;
    ActorLifecycleState lifecycle;
    AppearanceSync::RemoteState appearance;
    EquipmentSync::RemoteState equipment;
    FurnitureSync::RemoteState furniture;
};

using Peers = std::unordered_map<Multiplayer::PeerSlot, PeerState>;
