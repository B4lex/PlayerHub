#pragma once

#include <RE/Skyrim.h>

struct RemoteActor;

namespace ActorFurniture
{
    // What holds back a requested entry once the reference has resolved.
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
        // The native get-up was accepted. Its exit idle holds the graph and
        // transform until the clip ends and the engine releases the furniture.
        kNative,
        // No native exit idle played; the furniture graph node is left outright.
        kForced
    };

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
        // These gates hold an entry back on every pass without an engine refusal
        // to report, so each is logged once when it starts holding the request.
        EntryGate loggedEntryGate{ EntryGate::kNone };
        // IdleFurnitureExit leaves the furniture graph node outright. It is the
        // fallback when no native exit idle plays, sent once per exit.
        bool furnitureExitGraphForced{ false };
        // Kept across target changes like the elapsed time, so a source returning
        // to its seat waits for the exit instead of restarting it.
        ExitPhase exitPhase{ ExitPhase::kNone };
        // Time since the engine released the furniture while the exit clip still
        // drives the actor.
        float exitReleaseElapsed{ 0.0F };
        // The graph left the furniture node (IdleFurnitureExit); only the clip's
        // get-up settle remains.
        bool exitClipEnded{ false };
        // The marker probe refused the target while another live actor holds a
        // marker reservation on it, so entering would stack both actors on it.
        bool targetHeldByOtherActor{ false };
        // Furniture the source already used when this actor was spawned. Its entry
        // played before the actor existed, so the actor is seated without it.
        RE::FormID resumeFurnitureRefID{ 0 };
    };

    // Sets reconstructable native TESFurniture occupancy. Zero requests exit.
    void SetFurnitureState(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    // Called for a new actor whose source already uses the furniture: its first
    // entry into it skips the entry idle.
    void ResumeFurniture(RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] RE::FormID GetTargetFurniture(const RemoteActor& a_remote);
    [[nodiscard]] bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote);
    [[nodiscard]] bool IsTargetHeldByOtherActor(const RemoteActor& a_remote);

    // Drops occupancy and the marker reservation the actor holds on its furniture.
    // Called before the reference is retired: a deleted actor keeps its reservation,
    // and the engine then refuses every entry point on that furniture to the
    // replacement actor until the reservation expires. Also called before a kill,
    // since a body is no longer driven out of its furniture.
    void ReleaseFurniture(RE::Actor& a_actor, RemoteActor& a_remote);

    // Whether the actor occupies, is entering, or holds a marker reservation on
    // the furniture.
    [[nodiscard]] bool HoldsFurniture(RE::Actor& a_actor, const RemoteActor& a_remote, RE::TESObjectREFR& a_furniture);

    [[nodiscard]] bool HasMarkerReservation(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture);

    // Whether Skyrim's entry-point probe finds a free marker for a_user.
    [[nodiscard]] bool HasAvailableMarker(RE::Actor& a_user, RE::TESObjectREFR& a_furniture);
    // Whether the probe finds a free marker for a_user other than the one
    // a_occupant is seated on.
    [[nodiscard]] bool HasAvailableMarkerBeside(
        RE::Actor& a_user, RE::Actor& a_occupant, RE::TESObjectREFR& a_furniture);

    // Actor-module internal, called by ActorRuntime. Returns true while native
    // furniture owns the actor's placement, heading, and graph.
    [[nodiscard]] bool AdvanceFurnitureInteraction(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
