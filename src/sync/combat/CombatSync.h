#pragma once

#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <cstdint>
#include <deque>
#include <optional>

struct RemoteActor;

namespace CombatSync
{
    struct PendingAnimationAction
    {
        Network::AnimationAction action;
        float age{ 0.0F };
        // Time since its anchor checkpoint played.
        float dueElapsed{ 0.0F };
        std::uint32_t dispatchAttempts{ 0 };
        float retryElapsed{ 0.0F };
    };

    struct RemoteState
    {
        std::deque<PendingAnimationAction> pendingAnimationActions;
        float attackSpeedScale{ 1.0F };
        float attackCatchUpPeakScale{ 1.0F };
    };

    [[nodiscard]] std::optional<Network::Attack> CaptureLocalAttack(const LocalFrame& a_local);
    [[nodiscard]] bool CaptureLocalHandPresentation(const LocalFrame& a_local);
    [[nodiscard]] Multiplayer::HandPresentation SampleLocalHandPresentation();
    void QueueLocalAttacks(std::optional<Network::Attack> a_capturedAttack, bool a_graphEpochChanged);
    void DiscardLocalAttacks();
    void PublishLocalAttacks(const Network::PlayerTransform& a_checkpoint);

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);
    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action);

    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta);
    void ClearRemoteActions(RemoteState& a_state);
    void ClearRemoteActions(RemoteState& a_state, RemoteActor& a_actor);
    void SyncCombat(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_handsSettled);
    void SyncHandPresentation(const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_handsSettled, float a_delta);
}
