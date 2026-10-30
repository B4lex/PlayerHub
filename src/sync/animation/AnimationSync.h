#pragma once

#include "protocol/MovementState.h"
#include "sync/SyncFrame.h"
#include "sync/movement/MovementSync.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace AnimationSync
{
    struct LocalGraphEpoch
    {
        std::uint64_t epoch{ 1 };
        bool changed{ false };
    };

    struct LocalJump
    {
        bool replicated{ false };
        bool observedMidair{ false };
        bool waitForGraphReset{ false };
    };

    struct LocalState
    {
        // Compared only, never dereferenced.
        RE::BSAnimationGraphManager* observedGraph{ nullptr };
        std::uint64_t graphEpoch{ 1 };
        bool graphObserved{ false };
        bool graphContinuityLost{ false };

        LocalJump jump;
        float notMovingElapsed{ 0.0F };
        bool holdingArrival{ false };
        RE::NiPoint3 arrivalPosition{};
        Multiplayer::AnimationState published{};
        bool hasPublished{ false };
    };

    [[nodiscard]] LocalGraphEpoch ObserveLocalGraphEpoch(LocalState& a_state, const LocalFrame& a_local);
    void ObserveLocalMovement(LocalState& a_state, const LocalFrame& a_local, float a_delta);

    [[nodiscard]] Multiplayer::AnimationState SampleLocalAnimation(LocalState& a_state, const LocalFrame& a_local,
        std::uint64_t a_sequence, const MovementSync::LocalSample& a_movement);

    void OnCheckpointPlayed(const CheckpointUpdate& a_update);

    void SyncAnimation(const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_readyForWeaponDraw, float a_delta);

    // The played checkpoint's state as the actor can show it now.
    [[nodiscard]] Multiplayer::AnimationState GetPresentableState(
        const RemoteFrame& a_remote, const RemoteActor& a_actor, bool a_readyForWeaponDraw);
}
