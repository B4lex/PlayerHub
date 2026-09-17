#pragma once

#include <RE/Skyrim.h>

struct RemoteActor;

namespace ActorFurniture
{
    // Skyrim-native sit/sleep lifecycle of one remote actor.
    struct State
    {
        RE::FormID targetFurnitureRefID{ 0 };
        RE::FormID requestedFurnitureRefID{ 0 };
        float furnitureRetryRemaining{ 0.0F };
        float furnitureTransitionElapsed{ 0.0F };
        RE::SIT_SLEEP_STATE loggedSitSleepState{ RE::SIT_SLEEP_STATE::kNormal };
        // Skyrim's native furniture interaction owns the graph until the actor has
        // fully returned to its normal sit/sleep state. During that interval the
        // firewall admits locally generated furniture events; no event name is
        // accepted from a peer.
        bool furnitureAnimationOwned{ false };
    };

    // Sets reconstructable native TESFurniture occupancy. Zero requests exit.
    void SetFurnitureState(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] RE::FormID GetTargetFurniture(const RemoteActor& a_remote);
    [[nodiscard]] bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote);

    // Actor-module internal, called by ActorRuntime. Returns true while native
    // furniture owns the actor's placement, heading, and graph.
    [[nodiscard]] bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
