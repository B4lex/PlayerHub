#include "sync/CheckpointPlayout.h"
#include "protocol/Quantization.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    constexpr double kNominalIntervalSeconds = 0.05;
    constexpr double kSampleTimePeriodSeconds = Network::kSampleTimeModulus / 1000.0;
    constexpr double kInitialJitterSeconds = 0.025;
    constexpr double kJitterRiseSmoothing = 0.25;
    constexpr double kJitterDecaySmoothing = 0.02;
    constexpr double kIntervalSmoothing = 0.1;
    constexpr double kJitterMargin = 2.0;
    constexpr double kFrameMarginSeconds = 0.02;
    constexpr double kMinimumDelaySeconds = 0.05;
    constexpr double kMaximumDelaySeconds = 0.5;
    constexpr double kTransitFloorRisePerSecond = 0.01;
    constexpr double kMaximumSkew = 0.1;
    constexpr double kResyncSeconds = 0.25;
    constexpr double kMaximumSegmentSeconds = 0.25;
    constexpr float kMinimumTravelSpeed = 1.0F;
    constexpr std::size_t kMaximumPending = 16;
    constexpr float kTwoPi = 6.28318530717958647692F;
    // ActorAnimation cross-blends a Direction change of at least this much, as Skyrim does
    // for the player, so stepping it there keeps the blend instead of sweeping the clips.
    constexpr float kMaximumInterpolatedDirectionChange = 0.125F;
    constexpr double kDryLogSeconds = 0.02;
    // Above an idle player's drift of about 20 units per second.
    constexpr float kDryLogMinimumSpeed = 50.0F;
    constexpr std::uint32_t kInitialDryLogs = 10;
    constexpr std::uint32_t kDryLogInterval = 100;

    double ToSeconds(CheckpointPlayout::Clock::time_point a_time)
    {
        return std::chrono::duration<double>(a_time.time_since_epoch()).count();
    }

    float InterpolateDirection(
        const Network::PlayerTransform& a_from, const Network::PlayerTransform& a_to, float a_fraction)
    {
        if (!Multiplayer::IsMoving(a_from.animationState.movementState)
            || !Multiplayer::IsMoving(a_to.animationState.movementState))
        {
            return a_from.direction;
        }
        const float change = std::remainder(a_to.direction - a_from.direction, 1.0F);
        if (std::abs(change) >= kMaximumInterpolatedDirectionChange)
        {
            return a_from.direction;
        }
        return Multiplayer::NormalizeMovementDirection(a_from.direction + change * a_fraction);
    }

    RE::NiPoint3 GetPosition(const Network::PlayerTransform& a_checkpoint)
    {
        return { a_checkpoint.x, a_checkpoint.y, a_checkpoint.z };
    }

    bool CanInterpolate(const Network::PlayerTransform& a_from, const Network::PlayerTransform& a_to)
    {
        return a_from.mounted == a_to.mounted
            && MovementSync::IsSameCoordinateSpace(a_from.cellID, a_from.worldspaceID, a_to.cellID, a_to.worldspaceID)
            && GetPosition(a_from).GetDistance(GetPosition(a_to)) <= MovementSync::kRelocationDistance;
    }
}

bool CheckpointPlayout::Push(Network::PlayerTransform&& a_checkpoint, Clock::time_point a_received)
{
    if (hasReceived && a_checkpoint.sequence <= receivedSequence)
    {
        return false;
    }

    const double arrival = ToSeconds(a_received);
    const std::uint32_t sampleTime = a_checkpoint.sampleTime % Network::kSampleTimeModulus;
    double time = sampleTime / 1000.0;
    if (!hasReceived)
    {
        transitFloor = arrival - time;
        jitter = kInitialJitterSeconds;
        interval = kNominalIntervalSeconds;
    }
    else
    {
        const double wrapped
            = ((sampleTime + Network::kSampleTimeModulus - receivedSampleTime) % Network::kSampleTimeModulus) / 1000.0;
        const double arrivalGap = (std::max)(arrival - receivedArrival, 0.0);
        const double wraps = (std::max)(std::round((arrivalGap - wrapped) / kSampleTimePeriodSeconds), 0.0);
        time = receivedTime + wrapped + wraps * kSampleTimePeriodSeconds;

        const double gap = time - receivedTime;
        if (gap > 0.0 && gap <= kMaximumSegmentSeconds)
        {
            interval += (gap - interval) * kIntervalSmoothing;
        }
        const double transit = arrival - time;
        transitFloor = (std::min)(transit, transitFloor + kTransitFloorRisePerSecond * arrivalGap);
        const double excess = transit - transitFloor;
        jitter += (excess - jitter) * (excess > jitter ? kJitterRiseSmoothing : kJitterDecaySmoothing);
    }

    hasReceived = true;
    receivedSequence = a_checkpoint.sequence;
    receivedSampleTime = sampleTime;
    receivedTime = time;
    receivedArrival = arrival;

    if (pending.capacity() < kMaximumPending)
    {
        pending.reserve(kMaximumPending);
    }
    if (pending.size() >= kMaximumPending)
    {
        // Only reachable while playback is stuck far behind; checkpoints are latest-wins, so
        // skipping the oldest is safe.
        pending.erase(pending.begin());
    }
    pending.push_back(Entry{ .checkpoint = std::move(a_checkpoint), .time = time });
    return true;
}

void CheckpointPlayout::Reset()
{
    // Keeps the pending allocation for the peer's next entry.
    auto storage = std::move(pending);
    storage.clear();
    *this = CheckpointPlayout{};
    pending = std::move(storage);
}

const Network::PlayerTransform* CheckpointPlayout::GetNewest() const noexcept
{
    if (!pending.empty())
    {
        return &pending.back().checkpoint;
    }
    return hasPlayed ? &played : nullptr;
}

double CheckpointPlayout::GetDelay() const noexcept
{
    return std::clamp(
        interval + kJitterMargin * jitter + kFrameMarginSeconds, kMinimumDelaySeconds, kMaximumDelaySeconds);
}

void CheckpointPlayout::AdvanceClock(Clock::time_point a_now)
{
    if (!hasReceived)
    {
        return;
    }

    const double now = ToSeconds(a_now);
    const double delay = GetDelay();
    const double target = now - transitFloor - delay;
    const double elapsed = clockStarted ? (std::max)(now - advancedAt, 0.0) : 0.0;
    advancedAt = now;
    if (!clockStarted)
    {
        clockStarted = true;
        playbackTime = target;
        return;
    }

    playbackTime += elapsed;
    const double error = target - playbackTime;
    if (std::abs(error) > kResyncSeconds)
    {
        logs::debug("Checkpoint playout resynchronized: error={:.3f}s delay={:.3f}s jitter={:.3f}s interval={:.3f}s "
                    "pending={}",
            error, delay, jitter, interval, pending.size());
        playbackTime = target;
        return;
    }
    const double maximumCorrection = kMaximumSkew * elapsed;
    playbackTime += std::clamp(error, -maximumCorrection, maximumCorrection);
}

bool CheckpointPlayout::IsNextDue() const noexcept
{
    return !pending.empty() && (!hasPlayed || pending.front().time <= playbackTime);
}

void CheckpointPlayout::PlayNext()
{
    played = std::move(pending.front().checkpoint);
    playedTime = pending.front().time;
    hasPlayed = true;
    pending.erase(pending.begin());
}

// The pose holds the played checkpoint until the next one arrives, so a moving actor
// stops and then catches up. Idle checkpoints are sparse by design and not counted.
void CheckpointPlayout::ObserveDryPlayout(Multiplayer::PeerSlot a_playerID)
{
    const bool nowDry = hasPlayed && pending.empty();
    if (nowDry == dry)
    {
        return;
    }
    dry = nowDry;
    if (dry)
    {
        dryStartedAt = advancedAt;
        dryWhileMoving = played.speed >= kDryLogMinimumSpeed;
        return;
    }
    const double duration = advancedAt - dryStartedAt;
    if (!dryWhileMoving || duration < kDryLogSeconds)
    {
        return;
    }
    const auto runs = ++dryRuns;
    if (runs <= kInitialDryLogs || runs % kDryLogInterval == 0)
    {
        logs::debug(
            "Checkpoint playout ran dry: player={} duration={:.3f}s delay={:.3f}s jitter={:.3f}s "
            "interval={:.3f}s sequence={} count={}",
            a_playerID, duration, GetDelay(), jitter, interval, static_cast<unsigned long long>(played.sequence), runs);
    }
}

void CheckpointPlayout::UpdatePose()
{
    pose = RemotePose{ .position = GetPosition(played),
        .heading = played.heading,
        .pitch = played.pitch,
        .speed = played.speed,
        .sampledSpeed = played.speed,
        .direction = played.direction };
    if (!hasPlayed || pending.empty() || !CanInterpolate(played, pending.front().checkpoint))
    {
        return;
    }

    const auto& next = pending.front();
    const auto from = GetPosition(played);
    const auto to = GetPosition(next.checkpoint);
    const float distance = from.GetDistance(to);
    double start = playedTime;
    if (next.time - playedTime > kMaximumSegmentSeconds)
    {
        const double travel
            = next.checkpoint.speed >= kMinimumTravelSpeed ? distance / next.checkpoint.speed : interval;
        start = next.time - std::clamp(travel, interval, next.time - playedTime);
    }
    const double duration = next.time - start;
    if (duration <= 0.0)
    {
        return;
    }

    const auto fraction = static_cast<float>(std::clamp((playbackTime - start) / duration, 0.0, 1.0));
    const float turn = std::remainder(next.checkpoint.heading - played.heading, kTwoPi);
    pose.position = from + (to - from) * fraction;
    pose.heading = played.heading + turn * fraction;
    pose.headingRate = fraction < 1.0F ? static_cast<float>(turn / duration) : 0.0F;
    pose.pitch = played.pitch + (next.checkpoint.pitch - played.pitch) * fraction;
    pose.speed = static_cast<float>(distance / duration);
    pose.sampledSpeed = played.speed + (next.checkpoint.speed - played.speed) * fraction;
    pose.direction = InterpolateDirection(played, next.checkpoint, fraction);
}
