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
            logs::info("Animation sync publishing local furniture state: sequence={} previous={:08X} furniture={:08X}",
                static_cast<unsigned long long>(a_sequence), previousFurnitureRefID, furnitureRefID);
            previousFurnitureRefID = furnitureRefID;
            hasPreviousFurnitureRefID = true;
        }
        return furnitureRefID;
    }

    void OnCheckpointAccepted(const CheckpointUpdate& a_update)
    {
        if (a_update.previous.furnitureRefID != a_update.checkpoint.furnitureRefID)
        {
            logs::info(
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
