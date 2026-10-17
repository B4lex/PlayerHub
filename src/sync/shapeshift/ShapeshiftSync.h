#pragma once

#include "protocol/PlayerState.h"
#include "protocol/Shapeshift.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <deque>

struct RemoteActor;

namespace ShapeshiftSync
{
    [[nodiscard]] Multiplayer::Shapeshift ObserveLocalForm(const LocalFrame& a_local);
    [[nodiscard]] Multiplayer::Shapeshift GetLocalForm();

    struct VampireLordState
    {
        bool levitating{ false };
        bool mistForm{ false };
        bool bats{ false };
    };

    // All false unless the local player is a vampire lord.
    [[nodiscard]] VampireLordState SampleLocalVampireLord(const LocalFrame& a_local);

    // Any thread: an input event the local player's graph accepted.
    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName);
    void QueueLocalActions(bool a_graphEpochChanged);
    void DiscardLocalActions();
    void PublishLocalActions(const Network::PlayerTransform& a_checkpoint);

    struct PendingAction
    {
        Network::AnimationAction action;
        float age{ 0.0F };
        std::uint32_t dispatchAttempts{ 0 };
        float retryElapsed{ 0.0F };
    };

    struct RemoteState
    {
        std::deque<PendingAction> pendingActions;
    };

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);
    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action);
    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta);
    void ClearRemoteActions(RemoteState& a_state);
    // A beast actor only.
    void SyncBeast(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, float a_delta);
}
