#pragma once

#include "actor/ActorMount.h"
#include "actor/RemoteActor.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct PeerState;

// A riding player is presented by spawning a vanilla mount under its remote
// actor and seating the actor on it at once. The mount only exists while the
// player rides: a dismount despawns it and respawns the actor on foot, and every
// retirement of the actor removes it.
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
        bool seated{ false };
        // The rider was seated once. From then on it is never driven on foot
        // again, even while it is being seated anew: its graph is in the riding
        // state, and only a respawn returns it to locomotion.
        bool carriesRider{ false };
    };

    // Local stage, called by CheckpointPublisher once per published checkpoint.
    // Returns the mount the local player rides, whose transform the checkpoint
    // then carries, or null.
    [[nodiscard]] RE::NiPointer<RE::Actor> SampleLocalMount(const LocalFrame& a_local, std::uint64_t a_sequence);

    // Remote stage for a peer with a living actor, before any stage that drives
    // it. Spawns, drives, and seats the actor on its mount while the newest
    // checkpoint rides, and retires both once it no longer does. Returns whether
    // the mount carries the actor, in which case no stage may drive it on foot;
    // UpdateRider updates it instead.
    [[nodiscard]] bool SyncMount(const RemoteFrame& a_remote, PeerState& a_peer, float a_delta);
    // Remote stage, after SyncMount returned true. Returns false when the actor
    // handle is no longer valid.
    [[nodiscard]] bool UpdateRider(PeerState& a_peer, float a_delta);
}
