#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <chrono>
#include <cstdint>
#include <vector>

// Plays one peer's checkpoints back on a delayed copy of the sender's clock.
// They arrive unevenly: the server batches them once per tick and replaces one
// that a newer one overtook, far peers are sent less often, and the network
// adds jitter. Played at their sampled spacing a little behind the newest,
// they give the actor an evenly paced path. The whole checkpoint waits, not
// only its transform, so locomotion, jumps, stance, and the actions anchored
// to it stay with the position they were sampled at.
class CheckpointPlayout
{
public:
    using Clock = std::chrono::steady_clock;

    // Keeps a checkpoint newer than every one received until its playback
    // time. Returns false for a stale one.
    bool Push(Network::PlayerTransform&& a_checkpoint, Clock::time_point a_received);

    // Advances playback to a_now, and passes every checkpoint it reaches, in
    // order, to a_play before that checkpoint becomes the played one. The first
    // checkpoint after a reset plays at once, so a peer appears without delay.
    template <class Play>
    void Advance(Multiplayer::PeerSlot a_playerID, Clock::time_point a_now, Play&& a_play)
    {
        AdvanceClock(a_now);
        while (IsNextDue())
        {
            a_play(CheckpointUpdate{ .playerID = a_playerID,
                .inserted = !hasPlayed,
                .previous = played,
                .previousSequence = played.sequence,
                .checkpoint = pending.front().checkpoint });
            PlayNext();
        }
        UpdatePose();
    }

    // Forgets every checkpoint and the clock, as before the first.
    void Reset();

    // Default-constructed, with sequence zero, until one plays.
    [[nodiscard]] const Network::PlayerTransform& GetPlayed() const noexcept { return played; }
    [[nodiscard]] const RemotePose& GetPose() const noexcept { return pose; }
    // The newest checkpoint received, played or not; null before the first.
    [[nodiscard]] const Network::PlayerTransform* GetNewest() const noexcept;

private:
    struct Entry
    {
        Network::PlayerTransform checkpoint;
        // Sender clock in seconds, unwrapped.
        double time{ 0.0 };
    };

    void AdvanceClock(Clock::time_point a_now);
    [[nodiscard]] bool IsNextDue() const noexcept;
    void PlayNext();
    void UpdatePose();
    [[nodiscard]] double GetDelay() const noexcept;

    // Oldest first, bounded.
    std::vector<Entry> pending;
    Network::PlayerTransform played{};
    double playedTime{ 0.0 };
    bool hasPlayed{ false };
    RemotePose pose{};

    bool hasReceived{ false };
    std::uint64_t receivedSequence{ 0 };
    std::uint32_t receivedSampleTime{ 0 };
    double receivedTime{ 0.0 };
    double receivedArrival{ 0.0 };

    // Local arrival minus sender time for the least delayed recent checkpoint:
    // the clock offset plus the fastest path through the network and server.
    double transitFloor{ 0.0 };
    // Smoothed delay of checkpoints beyond transitFloor.
    double jitter{ 0.0 };
    // Smoothed sender-time spacing of consecutive checkpoints.
    double interval{ 0.0 };

    bool clockStarted{ false };
    // Sender clock in seconds.
    double playbackTime{ 0.0 };
    double advancedAt{ 0.0 };
};
