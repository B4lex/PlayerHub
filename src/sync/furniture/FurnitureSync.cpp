#include "sync/furniture/FurnitureSync.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/RemoteActor.h"
#include "forms/FormLookup.h"

#include <format>
#include <string>
#include <utility>

namespace FurnitureSync
{
    namespace
    {
        std::uint32_t GetOccupiedFurnitureRefID(RE::Actor& a_actor)
        {
            auto furniture = a_actor.GetOccupiedFurniture().get();
            return furniture ? furniture->GetFormID() : 0;
        }

        std::string FormatReference(const Multiplayer::FormReference& a_reference)
        {
            return a_reference.IsEmpty() ? std::string{ "<none>" }
                                         : std::format("{}|{:06X}", a_reference.plugin, a_reference.localFormID);
        }

        RE::SIT_SLEEP_STATE GetSitSleepState(const RE::Actor& a_actor)
        {
            // AsActorState relocates the ActorState base, which moved on AE.
            const auto* actorState = a_actor.AsActorState();
            return actorState ? actorState->GetSitSleepState() : RE::SIT_SLEEP_STATE::kNormal;
        }
    }

    Multiplayer::FormReference SampleLocalFurniture(const LocalFrame& a_local, std::uint64_t a_sequence)
    {
        static RE::FormID previousFurnitureRefID = 0;
        static Multiplayer::FormReference furniture;
        static bool hasPreviousFurnitureRefID = false;
        static RE::SIT_SLEEP_STATE previousSitSleepState = RE::SIT_SLEEP_STATE::kNormal;

        // The occupancy clears only when the player's exit clip ends, 1.1 s after
        // standing up from a chair and 3.3 s from a table. Peers play their own
        // exit clip once the furniture is gone from the checkpoint, so it is
        // dropped when the exit starts and both clips play side by side.
        const auto sitSleepState = GetSitSleepState(a_local.player);
        const bool exiting = sitSleepState == RE::SIT_SLEEP_STATE::kWantToStand
            || sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake;
        const auto furnitureRefID = exiting ? 0 : GetOccupiedFurnitureRefID(a_local.player);
        const bool furnitureChanged = !hasPreviousFurnitureRefID || furnitureRefID != previousFurnitureRefID;
        if (furnitureChanged)
        {
            furniture
                = FormLookup::MakeFormReference(furnitureRefID ? RE::TESForm::LookupByID(furnitureRefID) : nullptr)
                      .value_or(Multiplayer::FormReference{});
        }
        if (furnitureChanged || sitSleepState != previousSitSleepState)
        {
            logs::debug(
                "Animation sync publishing local furniture state: sequence={} previous={:08X} furniture={:08X} "
                "reference={} occupied={:08X} sitSleepState={}",
                static_cast<unsigned long long>(a_sequence), previousFurnitureRefID, furnitureRefID,
                FormatReference(furniture), GetOccupiedFurnitureRefID(a_local.player),
                static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(sitSleepState));
            previousFurnitureRefID = furnitureRefID;
            previousSitSleepState = sitSleepState;
            hasPreviousFurnitureRefID = true;
        }
        return furniture;
    }

    bool IsHeldByLocalPlayer(const LocalFrame& a_local, RE::TESObjectREFR& a_furniture)
    {
        return GetOccupiedFurnitureRefID(a_local.player) == a_furniture.GetFormID()
            || ActorFurniture::HasMarkerReservation(a_local.player, a_furniture);
    }

    bool BlocksLocalActivation(const LocalFrame& a_local, RE::TESObjectREFR& a_furniture, RemoteActor& a_actor)
    {
        auto actorPtr = a_actor.handle.get();
        return actorPtr && ActorFurniture::HoldsFurniture(*actorPtr, a_actor, a_furniture)
            && !ActorFurniture::HasAvailableMarker(a_local.player, a_furniture);
    }

    const char* FindFurnitureYieldReason(const LocalFrame& a_local, const RemoteFrame& a_remote,
        RE::TESObjectREFR* a_localFurniture, RemoteActor* a_actor)
    {
        const auto furnitureRefID = a_remote.furnitureRefID;
        if (furnitureRefID == 0)
        {
            return nullptr;
        }
        if (a_localFurniture && a_localFurniture->GetFormID() == furnitureRefID)
        {
            RE::NiPointer<RE::Actor> actorPtr;
            if (a_actor)
            {
                actorPtr = a_actor->handle.get();
            }
            if (!actorPtr)
            {
                return "occupied by the local player";
            }
            // An actor seated beside the local player on a shared bench keeps its
            // seat, and one yet to enter takes another free seat. The probe waits
            // for readiness, as native entry does.
            if (!ActorFurniture::HoldsFurniture(*actorPtr, *a_actor, *a_localFurniture) && a_actor->ready
                && !ActorFurniture::HasAvailableMarkerBeside(*actorPtr, a_local.player, *a_localFurniture))
            {
                return "no marker free beside the local player";
            }
        }
        if (a_actor && ActorFurniture::GetTargetFurniture(*a_actor) == furnitureRefID
            && ActorFurniture::IsTargetHeldByOtherActor(*a_actor))
        {
            return "occupied by another actor";
        }
        return nullptr;
    }

    bool IsVacated(const LocalFrame& a_local, RE::FormID a_furnitureRefID)
    {
        auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_furnitureRefID);
        return furniture && !IsHeldByLocalPlayer(a_local, *furniture)
            && ActorFurniture::HasAvailableMarker(a_local.player, *furniture);
    }

    void OnActorSpawned(const RemoteFrame& a_remote, RemoteActor& a_actor)
    {
        ActorFurniture::ResumeFurniture(a_actor, a_remote.furnitureRefID);
    }

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update)
    {
        const auto& furniture = a_update.checkpoint.furniture;
        if (a_update.previous.furniture == furniture)
        {
            return;
        }

        const auto previousRefID = std::exchange(a_state.refID, FormLookup::ResolveFormID(furniture));
        if (!furniture.IsEmpty() && a_state.refID == 0)
        {
            logs::warn(
                "Animation sync could not resolve remote furniture: player={} sequence={} furniture={}; it is not "
                "in the local load order",
                a_update.playerID, static_cast<unsigned long long>(a_update.checkpoint.sequence),
                FormatReference(furniture));
            return;
        }
        logs::debug(
            "Animation sync accepted main-loop furniture target: player={} sequence={} previous={:08X} "
            "furniture={:08X} reference={}",
            a_update.playerID, static_cast<unsigned long long>(a_update.checkpoint.sequence), previousRefID,
            a_state.refID, FormatReference(furniture));
    }

    bool SyncFurniture(const RemoteFrame& a_remote, RemoteActor& a_actor)
    {
        bool furniturePlaying = ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        if (ActorFurniture::GetTargetFurniture(a_actor) != a_remote.furnitureRefID
            && (a_remote.furnitureRefID == 0 || furniturePlaying))
        {
            // Exit and direct furniture-to-furniture changes must update the
            // authoritative target immediately. The native lifecycle exits the
            // old marker before entering a replacement.
            ActorFurniture::SetFurnitureState(a_actor, a_remote.furnitureRefID);
            furniturePlaying = ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        }
        return furniturePlaying;
    }

    bool SyncFurnitureEntry(
        const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_attackPlaying, bool a_furniturePlaying)
    {
        if (a_remote.furnitureRefID != 0 && ActorFurniture::GetTargetFurniture(a_actor) != a_remote.furnitureRefID
            && !a_attackPlaying && !ActorAnimation::GetTargetState(a_actor).jumping
            && ActorAnimation::IsAnimationStateSettled(a_actor))
        {
            // First entry waits until the checkpoint's grounded locomotion and
            // weapon presentation have settled. Once assigned, furniture owns
            // graph and transform progression until exit completes.
            ActorFurniture::SetFurnitureState(a_actor, a_remote.furnitureRefID);
            return ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        }
        return a_furniturePlaying;
    }
}
