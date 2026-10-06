#pragma once

#include "protocol/FormReference.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace FurnitureSync
{
    struct RemoteState
    {
        RE::FormID refID{ 0 };
    };

    [[nodiscard]] Multiplayer::FormReference SampleLocalFurniture(const LocalFrame& a_local, std::uint64_t a_sequence);

    [[nodiscard]] bool IsHeldByLocalPlayer(const LocalFrame& a_local, RE::TESObjectREFR& a_furniture);

    [[nodiscard]] bool BlocksLocalActivation(
        const LocalFrame& a_local, RE::TESObjectREFR& a_furniture, RemoteActor& a_actor);

    [[nodiscard]] const char* FindFurnitureYieldReason(const LocalFrame& a_local, const RemoteFrame& a_remote,
        RE::TESObjectREFR* a_localFurniture, RemoteActor* a_actor);

    [[nodiscard]] bool IsVacated(const LocalFrame& a_local, RE::FormID a_furnitureRefID);

    void OnActorSpawned(const RemoteFrame& a_remote, RemoteActor& a_actor);

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);

    [[nodiscard]] bool SyncFurniture(const RemoteFrame& a_remote, RemoteActor& a_actor);

    [[nodiscard]] bool SyncFurnitureEntry(
        const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_attackPlaying, bool a_furniturePlaying);
}
