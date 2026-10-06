#pragma once

#include <RE/Skyrim.h>

struct RemoteActor;

namespace ActorFurniture
{
    enum class EntryGate : std::uint8_t
    {
        kNone,
        kAnimationUnsettled,
        kAnimationBlocked,
        kAlignment,
        kMarkerDistance
    };

    enum class ExitPhase : std::uint8_t
    {
        kNone,
        kNative,
        kForced
    };

    struct State
    {
        RE::FormID targetFurnitureRefID{ 0 };
        RE::FormID requestedFurnitureRefID{ 0 };
        float furnitureRetryRemaining{ 0.0F };
        float furnitureTransitionElapsed{ 0.0F };
        RE::SIT_SLEEP_STATE loggedSitSleepState{ RE::SIT_SLEEP_STATE::kNormal };
        bool furnitureAnimationOwned{ false };
        // Native entry reports success before the occupancy it records is
        // observable, and sometimes records none at all.
        bool occupancyObserved{ false };
        bool entrySettleLogged{ false };
        EntryGate loggedEntryGate{ EntryGate::kNone };
        // IdleFurnitureExit leaves the furniture graph node outright.
        bool furnitureExitGraphForced{ false };
        ExitPhase exitPhase{ ExitPhase::kNone };
        float exitReleaseElapsed{ 0.0F };
        bool exitClipEnded{ false };
        bool targetHeldByOtherActor{ false };
        RE::FormID resumeFurnitureRefID{ 0 };
    };

    void SetFurnitureState(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    void ResumeFurniture(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] RE::FormID GetTargetFurniture(const RemoteActor& a_remote);
    [[nodiscard]] bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote);
    [[nodiscard]] bool IsTargetHeldByOtherActor(const RemoteActor& a_remote);

    // A deleted actor keeps its marker reservation, and the engine then refuses
    // every entry point on that furniture to a replacement actor until it expires.
    void ReleaseFurniture(RE::Actor& a_actor, RemoteActor& a_remote);

    [[nodiscard]] bool HoldsFurniture(RE::Actor& a_actor, const RemoteActor& a_remote, RE::TESObjectREFR& a_furniture);

    [[nodiscard]] bool HasMarkerReservation(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture);

    [[nodiscard]] bool HasAvailableMarker(RE::Actor& a_user, RE::TESObjectREFR& a_furniture);
    [[nodiscard]] bool HasAvailableMarkerBeside(
        RE::Actor& a_user, RE::Actor& a_occupant, RE::TESObjectREFR& a_furniture);

    [[nodiscard]] bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
