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
    bool ready{ false };
};
