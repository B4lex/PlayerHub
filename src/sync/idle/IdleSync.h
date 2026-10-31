#pragma once

#include "protocol/IdleEvent.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <deque>

struct RemoteActor;

namespace IdleSync
{
    // Any thread: an input event the local player's graph accepted.
    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName);
    void QueueLocalIdles(const LocalFrame& a_local, bool a_transformed, bool a_graphEpochChanged, float a_delta);
    void DiscardLocalIdles();

    struct LocalSample
    {
        Multiplayer::AnimationState animationState{};
        bool furniture{ false };
        bool mounted{ false };
        bool dead{ false };
        bool swimming{ false };
        bool transformed{ false };
    };

    // The idle the sampled checkpoint holds; a state change the idle cannot outlast
    // ends it first.
    [[nodiscard]] Multiplayer::IdleEvent SampleLocalIdle(const LocalSample& a_sample);
    void PublishLocalIdles(const Network::PlayerTransform& a_checkpoint);

    struct PendingIdle
    {
        Network::AnimationAction action;
        RE::BSFixedString event;
        bool exit{ false };
        float age{ 0.0F };
        std::uint32_t dispatchAttempts{ 0 };
        float retryElapsed{ 0.0F };
    };

    struct RemoteState
    {
        std::deque<PendingIdle> pendingIdles;
        // The played checkpoints' held idle, and the sequence of the first that showed
        // it, which names this instance of it.
        Multiplayer::IdleEvent heldEvent;
        RE::BSFixedString held;
        std::uint64_t heldInstance{ 0 };
        float heldElapsed{ 0.0F };
        std::uint32_t heldAttempts{ 0 };
        float heldRetryElapsed{ 0.0F };
        // The held instance that ended, until an exit reached the actor or the
        // backstop released it.
        std::uint64_t endedInstance{ 0 };
        float endedElapsed{ 0.0F };
        // A played state change that ends an idle, and the checkpoint that made it.
        const char* interruption{ nullptr };
        std::uint64_t interruptionSequence{ 0 };
        // Only throttles the log.
        std::uint32_t rejectedEvents{ 0 };
    };

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);
    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action);
    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta);
    void ClearRemoteActions(RemoteState& a_state);
    // A human actor on foot, outside furniture. a_attackQueued: an attack waits to play.
    void SyncIdle(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_handsSettled,
        bool a_attackQueued);
}
