#pragma once

#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMovement.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <memory>

// The actor reference replicating one remote player. Created by ActorSpawner,
// updated by ActorRuntime, and accessed only on the bound main-loop thread.
// The handle may become invalid at any time; check it before dereferencing.
struct RemoteActor
{
    RE::ActorHandle handle{};
    // Set while the actor is registered with ActorRegistry.
    RE::IAnimationGraphManagerHolder* animationGraphHolder{ nullptr };
    ActorMovement::State movement;
    // Allocated separately because its atomics are not movable. Never null for
    // an actor created by ActorSpawner.
    std::unique_ptr<ActorAnimation::State> animation;
    ActorFurniture::State furniture;
    // Main-loop passes spent waiting for 3D, controller, and graph; only used to
    // throttle initialization logs.
    std::uint32_t initializationWaitFrames{ 0 };
    // Seconds the actor has been ready, saturated once it is past every settling
    // gate. An actor created in the pass a cell load finished still has its
    // package, equipment, and AI process being installed around it.
    float readyElapsed{ 0.0F };
    bool ready{ false };
    // Killed for its player's death and left to Skyrim's death and ragdoll. Such
    // an actor is never driven again; a revived player gets a new actor.
    bool dead{ false };
};
