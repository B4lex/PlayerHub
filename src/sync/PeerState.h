#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPlayout.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/effects/EffectSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/idle/IdleSync.h"
#include "sync/movement/MovementSync.h"
#include "sync/nameplates/NameplateSync.h"
#include "sync/shapeshift/ShapeshiftSync.h"
#include "sync/skeleton/SkeletonSync.h"
#include "sync/voice/VoiceSync.h"

#include <cstdint>
#include <unordered_map>

struct PeerState
{
    CheckpointPlayout playout;
    std::uint64_t appliedSequence{ 0 };
    MovementSync::RemoteState space;
    CombatSync::RemoteState combat;
    ShapeshiftSync::RemoteState shapeshift;
    IdleSync::RemoteState idle;
    VoiceSync::RemoteState voice;
    ActorLifecycleState lifecycle;
    AppearanceSync::RemoteState appearance;
    EquipmentSync::RemoteState equipment;
    EffectSync::RemoteState effects;
    FurnitureSync::RemoteState furniture;
    NameplateSync::RemoteState nameplate;
    SkeletonSync::RemoteState skeleton;
};

using Peers = std::unordered_map<Multiplayer::PeerSlot, PeerState>;
