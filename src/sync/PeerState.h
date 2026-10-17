#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPlayout.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/nameplates/NameplateSync.h"
#include "sync/shapeshift/ShapeshiftSync.h"

#include <cstdint>
#include <unordered_map>

struct PeerState
{
    CheckpointPlayout playout;
    std::uint64_t appliedSequence{ 0 };
    CombatSync::RemoteState combat;
    ShapeshiftSync::RemoteState shapeshift;
    ActorLifecycleState lifecycle;
    AppearanceSync::RemoteState appearance;
    EquipmentSync::RemoteState equipment;
    FurnitureSync::RemoteState furniture;
    NameplateSync::RemoteState nameplate;
};

using Peers = std::unordered_map<Multiplayer::PeerSlot, PeerState>;
