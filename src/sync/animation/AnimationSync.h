#pragma once

#include "protocol/MovementState.h"
#include "sync/SyncFrame.h"
#include "sync/movement/MovementSync.h"

#include <cstdint>
#include <optional>

struct RemoteActor;

namespace AnimationSync
{
    struct LocalGraphEpoch
    {
        std::uint64_t epoch{ 1 };
        bool changed{ false };
    };

    [[nodiscard]] LocalGraphEpoch ObserveLocalGraphEpoch(const LocalFrame& a_local);
    void ObserveLocalMovementInput(const LocalFrame& a_local, float a_delta);

    [[nodiscard]] Multiplayer::AnimationState SampleLocalAnimation(
        const LocalFrame& a_local, std::uint64_t a_sequence, const MovementSync::LocalSample& a_movement);

    void OnCheckpointPlayed(const CheckpointUpdate& a_update);

    void SyncAnimation(const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_readyForWeaponDraw, float a_delta);

    [[nodiscard]] std::optional<Multiplayer::AnimationState> GetTranslationState(
        const RemoteFrame& a_remote, const RemoteActor& a_actor, bool a_readyForWeaponDraw, bool a_furniturePlaying);
}
