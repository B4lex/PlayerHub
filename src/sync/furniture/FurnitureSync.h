#pragma once

#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace FurnitureSync
{
    // Local stage, called by CheckpointPublisher once per published checkpoint.
    // Furniture and cell FormIDs require matching peer load orders.
    [[nodiscard]] RE::FormID SampleLocalFurniture(const LocalFrame& a_local, std::uint64_t a_sequence);

    // Receive stage: records a newer checkpoint's furniture target.
    void OnCheckpointAccepted(const CheckpointUpdate& a_update);

    // Remote stage, before animation staging: applies an exit or a direct
    // furniture-to-furniture change immediately. Returns whether native furniture
    // owns the actor's graph and transform.
    [[nodiscard]] bool SyncFurniture(const RemoteFrame& a_remote, RemoteActor& a_actor);

    // Remote stage, after animation staging: starts the first entry once the
    // checkpoint's grounded locomotion and weapon presentation have settled.
    // Returns the updated furniture ownership result.
    [[nodiscard]] bool SyncFurnitureEntry(
        const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_attackPlaying, bool a_furniturePlaying);
}
