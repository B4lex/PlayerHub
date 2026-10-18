#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/PlayerState.h"
#include "sync/SyncFrame.h"

#include <chrono>
#include <cstdint>
#include <vector>

class CheckpointPlayout
{
public:
    using Clock = std::chrono::steady_clock;

    bool Push(Network::PlayerTransform&& a_checkpoint, Clock::time_point a_received);

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
        ObserveDryPlayout(a_playerID);
    }

    void Reset();

    [[nodiscard]] const Network::PlayerTransform& GetPlayed() const noexcept { return played; }
    [[nodiscard]] const RemotePose& GetPose() const noexcept { return pose; }
    [[nodiscard]] const Network::PlayerTransform* GetNewest() const noexcept;

private:
    struct Entry
    {
        Network::PlayerTransform checkpoint;
        double time{ 0.0 };
    };

    void AdvanceClock(Clock::time_point a_now);
    [[nodiscard]] bool IsNextDue() const noexcept;
    void PlayNext();
    void UpdatePose();
    void ObserveDryPlayout(Multiplayer::PeerSlot a_playerID);
    [[nodiscard]] double GetDelay() const noexcept;

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

    double transitFloor{ 0.0 };
    double jitter{ 0.0 };
    double interval{ 0.0 };

    bool clockStarted{ false };
    double playbackTime{ 0.0 };
    double advancedAt{ 0.0 };

    bool dry{ false };
    bool dryWhileMoving{ false };
    double dryStartedAt{ 0.0 };
    std::uint32_t dryRuns{ 0 };
};
