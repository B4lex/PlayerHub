#pragma once

#include "protocol/FormReference.h"
#include "protocol/MovementState.h"
#include "protocol/PlayerState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// The wire's quantized checkpoint and its context, shared by the publisher and
// the receiver so both apply the same rules as the server
// (server/internal/wire/quantize.go).
namespace Network
{
    inline constexpr std::uint32_t kHeadingSteps = 4096;
    inline constexpr float kPitchScale = 1024.0F;
    inline constexpr std::int32_t kMaximumQuantizedPitch = 1609;
    inline constexpr std::uint32_t kMaximumQuantizedSpeed = 100000;
    inline constexpr std::uint32_t kDirectionSteps = 256;

    // Checkpoint.flags: bits 0-2 hold the movement state.
    namespace CheckpointFlags
    {
        inline constexpr std::uint32_t kMovementStateMask = 0x7;
        inline constexpr std::uint32_t kJumping = 1U << 3;
        inline constexpr std::uint32_t kWeaponDrawn = 1U << 4;
        inline constexpr std::uint32_t kBowDrawn = 1U << 5;
        inline constexpr std::uint32_t kCastingLeft = 1U << 6;
        inline constexpr std::uint32_t kCastingRight = 1U << 7;
        inline constexpr std::uint32_t kBlocking = 1U << 8;
        inline constexpr std::uint32_t kDead = 1U << 9;
        inline constexpr std::uint32_t kMounted = 1U << 10;
        inline constexpr std::uint32_t kSwimming = 1U << 11;
        inline constexpr std::uint32_t kKnown = 0xFFF;
    }

    struct QuantizedCheckpoint
    {
        std::uint64_t sequence{ 0 };
        // Whole game units.
        std::int32_t x{ 0 };
        std::int32_t y{ 0 };
        std::int32_t z{ 0 };
        // 1/4096 of a turn, 0..4095.
        std::uint32_t heading{ 0 };
        // 1/1024 radian.
        std::int32_t pitch{ 0 };
        // Whole units per second.
        std::uint32_t speed{ 0 };
        // 1/256 of the graph's Direction range, 0..255.
        std::uint32_t direction{ 0 };
        std::uint32_t flags{ 0 };
    };

    // What a checkpoint's position and actions are relative to. It changes
    // rarely, so it travels separately and only when it changes.
    struct CheckpointContext
    {
        std::uint32_t cellID{ 0 };
        std::uint32_t worldspaceID{ 0 };
        std::uint64_t graphEpoch{ 0 };
        Multiplayer::FormReference furniture;

        bool operator==(const CheckpointContext&) const = default;
    };

    namespace Detail
    {
        [[nodiscard]] inline std::int32_t RoundToInt32(float a_value) noexcept
        {
            if (!std::isfinite(a_value))
            {
                return 0;
            }
            const double rounded = std::round(static_cast<double>(a_value));
            return static_cast<std::int32_t>(std::clamp(rounded,
                static_cast<double>((std::numeric_limits<std::int32_t>::min)()),
                static_cast<double>((std::numeric_limits<std::int32_t>::max)())));
        }
    }

    [[nodiscard]] inline CheckpointContext GetCheckpointContext(const PlayerTransform& a_transform)
    {
        return CheckpointContext{ .cellID = a_transform.cellID,
            .worldspaceID = a_transform.worldspaceID,
            .graphEpoch = a_transform.graphEpoch,
            .furniture = a_transform.furniture };
    }

    [[nodiscard]] inline QuantizedCheckpoint Quantize(const PlayerTransform& a_transform)
    {
        using namespace CheckpointFlags;
        constexpr float kTwoPi = 6.28318530717958647692F;
        float heading = std::isfinite(a_transform.heading) ? std::fmod(a_transform.heading, kTwoPi) : 0.0F;
        if (heading < 0.0F)
        {
            heading += kTwoPi;
        }
        const auto pitch = std::clamp(Detail::RoundToInt32(a_transform.pitch * kPitchScale), -kMaximumQuantizedPitch,
            kMaximumQuantizedPitch);
        const float speed = std::isfinite(a_transform.speed) ? a_transform.speed : 0.0F;
        const float direction = Multiplayer::NormalizeMovementDirection(a_transform.direction);

        std::uint32_t flags = static_cast<std::uint32_t>(a_transform.animationState.movementState) & kMovementStateMask;
        const auto set = [&flags](bool a_value, std::uint32_t a_flag) {
            if (a_value)
            {
                flags |= a_flag;
            }
        };
        set(a_transform.animationState.jumping, kJumping);
        set(a_transform.animationState.weaponDrawn, kWeaponDrawn);
        set(a_transform.handPresentation.bowDrawn, kBowDrawn);
        set(a_transform.handPresentation.castingLeft, kCastingLeft);
        set(a_transform.handPresentation.castingRight, kCastingRight);
        set(a_transform.handPresentation.blocking, kBlocking);
        set(a_transform.dead, kDead);
        set(a_transform.mounted, kMounted);
        set(a_transform.swimming, kSwimming);

        return QuantizedCheckpoint{ .sequence = a_transform.sequence,
            .x = Detail::RoundToInt32(a_transform.x),
            .y = Detail::RoundToInt32(a_transform.y),
            .z = Detail::RoundToInt32(a_transform.z),
            .heading = static_cast<std::uint32_t>(std::lround(heading / kTwoPi * kHeadingSteps)) % kHeadingSteps,
            .pitch = pitch,
            .speed = static_cast<std::uint32_t>(std::clamp(std::round(speed), 0.0F,
                static_cast<float>(kMaximumQuantizedSpeed))),
            .direction = static_cast<std::uint32_t>(std::lround(direction * kDirectionSteps)) % kDirectionSteps,
            .flags = flags };
    }

    // Rejects values outside the server's validation, as the server does.
    [[nodiscard]] inline bool IsValidQuantizedCheckpoint(const QuantizedCheckpoint& a_checkpoint) noexcept
    {
        using namespace CheckpointFlags;
        return a_checkpoint.heading < kHeadingSteps && a_checkpoint.pitch >= -kMaximumQuantizedPitch
            && a_checkpoint.pitch <= kMaximumQuantizedPitch && a_checkpoint.speed <= kMaximumQuantizedSpeed
            && a_checkpoint.direction < kDirectionSteps && (a_checkpoint.flags & ~kKnown) == 0
            && (a_checkpoint.flags & kMovementStateMask)
                   <= static_cast<std::uint32_t>(Multiplayer::MovementState::kSneakingRunning);
    }

    [[nodiscard]] inline PlayerTransform Dequantize(
        const QuantizedCheckpoint& a_checkpoint, const CheckpointContext& a_context)
    {
        using namespace CheckpointFlags;
        constexpr float kTwoPi = 6.28318530717958647692F;
        const auto flags = a_checkpoint.flags;
        return PlayerTransform{ .x = static_cast<float>(a_checkpoint.x),
            .y = static_cast<float>(a_checkpoint.y),
            .z = static_cast<float>(a_checkpoint.z),
            .heading = static_cast<float>(a_checkpoint.heading) * kTwoPi / static_cast<float>(kHeadingSteps),
            .pitch = static_cast<float>(a_checkpoint.pitch) / kPitchScale,
            .speed = static_cast<float>(a_checkpoint.speed),
            .direction = static_cast<float>(a_checkpoint.direction) / static_cast<float>(kDirectionSteps),
            .animationState = Multiplayer::AnimationState{
                .movementState = static_cast<Multiplayer::MovementState>(flags & kMovementStateMask),
                .jumping = (flags & kJumping) != 0,
                .weaponDrawn = (flags & kWeaponDrawn) != 0 },
            .handPresentation = Multiplayer::HandPresentation{ .bowDrawn = (flags & kBowDrawn) != 0,
                .castingLeft = (flags & kCastingLeft) != 0,
                .castingRight = (flags & kCastingRight) != 0,
                .blocking = (flags & kBlocking) != 0 },
            .dead = (flags & kDead) != 0,
            .mounted = (flags & kMounted) != 0,
            .swimming = (flags & kSwimming) != 0,
            .cellID = a_context.cellID,
            .worldspaceID = a_context.worldspaceID,
            .furniture = a_context.furniture,
            .graphEpoch = a_context.graphEpoch,
            .sequence = a_checkpoint.sequence };
    }
}
