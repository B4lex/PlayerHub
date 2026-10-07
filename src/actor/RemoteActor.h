#pragma once

#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMovement.h"
#include "actor/RemoteCollision.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor
{
    RE::ActorHandle handle{};
    RE::IAnimationGraphManagerHolder* animationGraphHolder{ nullptr };
    ActorMovement::State movement;
    ActorAnimation::State animation;
    ActorFurniture::State furniture;
    // Main-loop passes, only used to throttle initialization logs.
    std::uint32_t initializationWaitFrames{ 0 };
    float readyElapsed{ 0.0F };
    bool ready{ false };
    RemoteCollision::State collision;
    bool dead{ false };
};
