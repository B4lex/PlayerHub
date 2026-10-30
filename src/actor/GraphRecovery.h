#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace GraphRecovery
{
    struct State
    {
        bool localInterruptionLatched{ false };
        bool inProgress{ false };
        bool used3DReset{ false };
        // Compared only, never dereferenced.
        RE::BSAnimationGraphManager* graphBeforeRecovery{ nullptr };
        std::uint32_t observedHitEvents{ 0 };
        float hitReactionObservationRemaining{ 0.0F };
        // Main-loop passes, only used to throttle logs.
        std::uint32_t waitFrames{ 0 };
        float waitElapsed{ 0.0F };
        std::uint32_t stableFrames{ 0 };
        std::uint32_t count{ 0 };
    };

    // Returns true while recovery owns the actor and its transform and graph writes wait.
    [[nodiscard]] bool Recover(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
