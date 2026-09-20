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
        // Native entry reports success before the occupancy it records is
        // observable, and sometimes records none at all. Until the target has been
        // seen occupied once, a started entry is unconfirmed and gets the short
        // retry budget instead of the one for an entry Skyrim dropped mid-idle.
        bool occupancyObserved{ false };
        bool entrySettleLogged{ false };
        // A target assigned while the actor was still settling from its spawn began
        // before the actor existed, behind the local loading screen. Its entry idle
        // already played on the source and was never visible here, so the actor
        // adopts the seated pose instead of replaying a sit the peer has finished.
        bool resumeWithoutEntryIdle{ false };
    };

    // Sets reconstructable native TESFurniture occupancy. Zero requests exit.
    void SetFurnitureState(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] RE::FormID GetTargetFurniture(const RemoteActor& a_remote);
    [[nodiscard]] bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote);

    // Drops occupancy and the marker reservation the actor holds on its furniture.
    // Called before the reference is retired: a deleted actor keeps its reservation,
    // and the engine then refuses every entry point on that furniture to the
    // replacement actor until the reservation expires.
    void ReleaseFurniture(RE::Actor& a_actor, RemoteActor& a_remote);

    // Actor-module internal, called by ActorRuntime. Returns true while native
    // furniture owns the actor's placement, heading, and graph.
    [[nodiscard]] bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
