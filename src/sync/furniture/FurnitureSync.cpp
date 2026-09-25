#include "sync/furniture/FurnitureSync.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/RemoteActor.h"

namespace FurnitureSync
{
    namespace
    {
        std::uint32_t GetOccupiedFurnitureRefID(RE::Actor& a_actor)
        {
            auto furniture = a_actor.GetOccupiedFurniture().get();
            return furniture ? furniture->GetFormID() : 0;
        }
    }

    RE::FormID SampleLocalFurniture(const LocalFrame& a_local, std::uint64_t a_sequence)
    {
        static RE::FormID previousFurnitureRefID = 0;
        static bool hasPreviousFurnitureRefID = false;

        const auto furnitureRefID = GetOccupiedFurnitureRefID(a_local.player);
        if (!hasPreviousFurnitureRefID || furnitureRefID != previousFurnitureRefID)
        {
            logs::debug("Animation sync publishing local furniture state: sequence={} previous={:08X} furniture={:08X}",
                static_cast<unsigned long long>(a_sequence), previousFurnitureRefID, furnitureRefID);
            previousFurnitureRefID = furnitureRefID;
            hasPreviousFurnitureRefID = true;
        }
        return furnitureRefID;
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

    const char* FindFurnitureYieldReason(
        const RemoteFrame& a_remote, RE::TESObjectREFR* a_localFurniture, RemoteActor* a_actor)
    {
        const auto furnitureRefID = a_remote.checkpoint.furnitureRefID;
        if (furnitureRefID == 0)
        {
            return nullptr;
        }
        if (a_localFurniture && a_localFurniture->GetFormID() == furnitureRefID)
        {
            // An actor seated beside the local player on a shared bench keeps its seat.
            RE::NiPointer<RE::Actor> actorPtr;
            if (a_actor)
            {
                actorPtr = a_actor->handle.get();
            }
            if (!actorPtr || !ActorFurniture::HoldsFurniture(*actorPtr, *a_actor, *a_localFurniture))
            {
                return "occupied by the local player";
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
        ActorFurniture::ResumeFurniture(a_actor, a_remote.checkpoint.furnitureRefID);
    }

    void OnCheckpointAccepted(const CheckpointUpdate& a_update)
    {
        if (a_update.previous.furnitureRefID != a_update.checkpoint.furnitureRefID)
        {
            logs::debug(
                "Animation sync accepted main-loop furniture target: player={} sequence={} previous={:08X} "
                "furniture={:08X}",
                a_update.playerID, static_cast<unsigned long long>(a_update.checkpoint.sequence),
                a_update.previous.furnitureRefID, a_update.checkpoint.furnitureRefID);
        }
    }

    bool SyncFurniture(const RemoteFrame& a_remote, RemoteActor& a_actor)
    {
        bool furniturePlaying = ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        if (ActorFurniture::GetTargetFurniture(a_actor) != a_remote.checkpoint.furnitureRefID
            && (a_remote.checkpoint.furnitureRefID == 0 || furniturePlaying))
        {
            // Exit and direct furniture-to-furniture changes must update the
            // authoritative target immediately. The native lifecycle exits the
            // old marker before entering a replacement.
            ActorFurniture::SetFurnitureState(a_actor, a_remote.checkpoint.furnitureRefID);
            furniturePlaying = ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        }
        return furniturePlaying;
    }

    bool SyncFurnitureEntry(
        const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_attackPlaying, bool a_furniturePlaying)
    {
        if (a_remote.checkpoint.furnitureRefID != 0
            && ActorFurniture::GetTargetFurniture(a_actor) != a_remote.checkpoint.furnitureRefID && !a_attackPlaying
            && !ActorAnimation::GetTargetState(a_actor).jumping && ActorAnimation::IsAnimationStateSettled(a_actor))
        {
            // First entry waits until the checkpoint's grounded locomotion and
            // weapon presentation have settled. Once assigned, furniture owns
            // graph and transform progression until exit completes.
            ActorFurniture::SetFurnitureState(a_actor, a_remote.checkpoint.furnitureRefID);
            return ActorFurniture::IsFurnitureInteractionPlaying(a_actor);
        }
        return a_furniturePlaying;
    }
}
