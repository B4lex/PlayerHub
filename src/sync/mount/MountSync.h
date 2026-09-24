#pragma once

#include "actor/ActorMount.h"
#include "actor/RemoteActor.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

struct PeerState;

// A riding player is presented by spawning a vanilla mount under its remote
// actor and seating the actor on it. The mount only exists while the player
// rides: a dismount despawns it, and every retirement of the actor removes it.
//
// Getting on and off are separate events rather than checkpoint state. An enter
// received shortly before the seat plays Skyrim's mount animation, and an exit
// plays its dismount animation, after which the actor stays and continues on
// foot. Without one, the seat is instant, and a dismount respawns the actor on
// foot.
namespace MountSync
{
    struct RemoteMount
    {
        RemoteActor actor;
        ActorMount::Gait gait;
        // The rider's riding graph picks its gait from its own Speed.
        ActorMount::Gait riderGait;
        // Checkpoint sequence whose transform the mount last received.
        std::uint64_t appliedSequence{ 0 };
        float seatRetryRemaining{ 0.0F };
        float seatCheckRemaining{ 0.0F };
        std::uint32_t seatAttempts{ 0 };
        // Seconds since the mount was spawned, and since the rider was last seated.
        float spawnedElapsed{ 0.0F };
        float seatedElapsed{ 0.0F };
        bool seated{ false };
        // The last seat played the mount animation.
        bool animatedSeat{ false };
        // The rider was seated once. From then on it is never driven on foot
        // again, even while it is being seated anew: its graph is in the riding
        // state, and only a respawn or an animated dismount returns it to
        // locomotion.
        bool carriesRider{ false };
        // A mount or dismount clip was playing in this pass.
        bool transitionPlaying{ false };
        // Seconds the mount stays held after a clip ended.
        float clipSettleRemaining{ 0.0F };
        // An animated dismount started, and the seconds since.
        bool exiting{ false };
        float exitElapsed{ 0.0F };
        // The animated dismount ended with the rider standing beside its mount,
        // and the seconds since. The mount is removed once the newest checkpoint no
        // longer rides.
        bool dismounted{ false };
        float dismountedElapsed{ 0.0F };
    };

    // Receive state of one peer's mount transitions.
    struct RemoteState
    {
        struct PendingTransition
        {
            Network::MountTransition transition{ Network::MountTransition::kEnter };
            std::uint64_t checkpointSequence{ 0 };
            float age{ 0.0F };
        };

        // The newest accepted transition that has not started yet.
        std::optional<PendingTransition> pending;
        // Checkpoint sequence of the newest accepted transition, which orders them.
        std::uint64_t latestCheckpointSequence{ 0 };
    };

    // Local stage, called by CheckpointPublisher once per published checkpoint.
    // Returns the mount the local player rides, whose transform the checkpoint
    // then carries, or null. Observes the start of an animated mount or dismount.
    [[nodiscard]] RE::NiPointer<RE::Actor> SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence);
    // Publishes a transition observed by SampleLocalMount against the checkpoint
    // that was just published with it.
    void PublishLocalTransition(const Network::PlayerTransform& a_checkpoint);

    // Receive stage: a_state and a_checkpoint are null for an unknown player.
    void AcceptRemoteTransition(RemoteState* a_state, const Network::PlayerTransform* a_checkpoint,
        std::uint64_t a_lastSequence, const Network::RemoteMountTransition& a_transition);
    // Drops a transition that has not started, such as one received for a retired
    // actor.
    void DiscardRemoteTransition(RemoteState& a_state);

    // Remote stage for a peer with a living actor, before any stage that drives
    // it. Spawns, drives, and seats the actor on its mount while the newest
    // checkpoint rides, plays received transitions, and removes the mount once
    // the player no longer rides. Returns whether the mount owns the actor, in
    // which case no stage may drive it on foot; UpdateRider updates it instead.
    [[nodiscard]] bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    // Remote stage, after SyncMount returned true. Returns false when the actor
    // handle is no longer valid.
    [[nodiscard]] bool UpdateRider(PeerState& a_peer, float a_delta);
}
