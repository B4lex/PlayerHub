#pragma once

#include "protocol/MovementState.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <deque>

struct RemoteActor;

namespace VoiceSync
{
    void RegisterEventSinks();

    // Returns true when a shout starts, so equipment flushes its shout first.
    [[nodiscard]] bool CaptureLocalVoice(const LocalFrame& a_local, bool a_transformed);
    [[nodiscard]] Multiplayer::VoicePresentation SampleLocalVoice();
    void QueueLocalPowers(bool a_transformed, bool a_graphEpochChanged);
    void DiscardLocalVoice();
    void PublishLocalPowers(const Network::PlayerTransform& a_checkpoint);

    struct PendingPower
    {
        Network::AnimationAction action;
        float age{ 0.0F };
    };

    struct RemoteState
    {
        std::deque<PendingPower> pendingPowers;
        // Counts the source's shouts, so a shout the actor already played is told from
        // the next one, which may already be released by then.
        std::uint32_t shoutID{ 0 };
    };

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update);
    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action);
    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta);
    void ClearRemoteActions(RemoteState& a_state);
    // For an actor leaving the voice stage, such as one that mounts.
    void ClearRemote(RemoteState& a_state, RemoteActor& a_actor);
    // A human actor on foot, outside furniture. a_shout is the resolved shout of the
    // peer's equipment.
    void SyncVoice(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, RE::TESShout* a_shout,
        float a_delta);
}
