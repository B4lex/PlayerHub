#pragma once

#include "protocol/FormReference.h"
#include "protocol/MovementState.h"
#include "protocol/PlayerState.h"
#include "protocol/SkeletonPose.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Network
{
    inline constexpr float kPositionScale = 4.0F;
    inline constexpr std::uint32_t kHeadingSteps = 4096;
    inline constexpr float kPitchScale = 1024.0F;
    inline constexpr std::int32_t kMaximumQuantizedPitch = 1609;
    inline constexpr std::uint32_t kMaximumQuantizedSpeed = 100000;
    inline constexpr std::uint32_t kDirectionSteps = 256;
    inline constexpr std::uint32_t kSampleTimeModulus = 16384;

    namespace CheckpointFlags
    {
        inline constexpr std::uint32_t kMovementStateMask = 0x7;
        inline constexpr std::uint32_t kGrounded = 1U << 3;
        inline constexpr std::uint32_t kWeaponDrawn = 1U << 4;
        inline constexpr std::uint32_t kJumping = 1U << 5;
        inline constexpr std::uint32_t kBlocking = 1U << 6;
        inline constexpr std::uint32_t kBowDrawn = 1U << 7;
        inline constexpr std::uint32_t kCastingLeft = 1U << 8;
        inline constexpr std::uint32_t kCastingRight = 1U << 9;
        inline constexpr std::uint32_t kSwimming = 1U << 10;
        inline constexpr std::uint32_t kMounted = 1U << 11;
        inline constexpr std::uint32_t kDead = 1U << 12;
        inline constexpr std::uint32_t kLevitating = 1U << 13;
        inline constexpr std::uint32_t kMistForm = 1U << 14;
        inline constexpr std::uint32_t kBats = 1U << 15;
        inline constexpr std::uint32_t kCastReleasedLeft = 1U << 16;
        inline constexpr std::uint32_t kCastReleasedRight = 1U << 17;
        inline constexpr std::uint32_t kDualCasting = 1U << 18;
        inline constexpr std::uint32_t kShouting = 1U << 19;
        inline constexpr std::uint32_t kShoutReleased = 1U << 20;
        inline constexpr std::uint32_t kShoutTwoWords = 1U << 21;
        inline constexpr std::uint32_t kShoutThreeWords = 1U << 22;
        inline constexpr std::uint32_t kShoutFailed = 1U << 23;
        inline constexpr std::uint32_t kFlyingMount = 1U << 24;
        inline constexpr std::uint32_t kFlyStateShift = 25;
        inline constexpr std::uint32_t kFlyStateMask = 0x7U << kFlyStateShift;
        inline constexpr std::uint32_t kWingStateShift = 28;
        inline constexpr std::uint32_t kWingStateMask = 0x3U << kWingStateShift;
        inline constexpr std::uint32_t kKnown = 0x3FFFFFFF;
    }

    struct QuantizedCheckpoint
    {
        std::uint64_t sequence{ 0 };
        std::int32_t x{ 0 };
        std::int32_t y{ 0 };
        std::int32_t z{ 0 };
        std::uint32_t heading{ 0 };
        std::int32_t pitch{ 0 };
        std::int32_t lookPitch{ 0 };
        std::uint32_t speed{ 0 };
        std::uint32_t direction{ 0 };
        std::uint32_t flags{ 0 };
        std::uint32_t time{ 0 };
        EncodedSkeletonPose skeleton;
    };

    struct CheckpointContext
    {
        Multiplayer::FormReference interiorCell;
        Multiplayer::FormReference worldspace;
        std::uint64_t graphEpoch{ 0 };
        Multiplayer::FormReference furniture;
        Multiplayer::Shapeshift shapeshift{ Multiplayer::Shapeshift::kNone };
        Multiplayer::IdleEvent idle;

        bool operator==(const CheckpointContext& a_other) const
        {
            return interiorCell == a_other.interiorCell && worldspace == a_other.worldspace
                && graphEpoch == a_other.graphEpoch && furniture == a_other.furniture
                && shapeshift == a_other.shapeshift && Multiplayer::IsSameIdleEvent(idle, a_other.idle);
        }
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

        [[nodiscard]] inline std::int32_t QuantizePitch(float a_pitch) noexcept
        {
            return std::clamp(RoundToInt32(a_pitch * kPitchScale), -kMaximumQuantizedPitch, kMaximumQuantizedPitch);
        }

        [[nodiscard]] constexpr bool IsValidQuantizedPitch(std::int32_t a_pitch) noexcept
        {
            return a_pitch >= -kMaximumQuantizedPitch && a_pitch <= kMaximumQuantizedPitch;
        }
    }

    [[nodiscard]] inline CheckpointContext GetCheckpointContext(const PlayerTransform& a_transform)
    {
        return CheckpointContext{ .interiorCell = a_transform.interiorCell,
            .worldspace = a_transform.worldspace,
            .graphEpoch = a_transform.graphEpoch,
            .furniture = a_transform.furniture,
            .shapeshift = a_transform.shapeshift,
            .idle = a_transform.idle };
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
        set(a_transform.handPresentation.castReleasedLeft, kCastReleasedLeft);
        set(a_transform.handPresentation.castReleasedRight, kCastReleasedRight);
        set(a_transform.handPresentation.dualCasting, kDualCasting);
        set(a_transform.handPresentation.blocking, kBlocking);
        const auto& voice = a_transform.voice;
        set(voice.shouting, kShouting);
        set(voice.shouting && voice.released, kShoutReleased);
        set(voice.shouting && voice.words == 2, kShoutTwoWords);
        set(voice.shouting && voice.words >= 3, kShoutThreeWords);
        set(voice.shouting && voice.released && voice.failed, kShoutFailed);
        set(a_transform.dead, kDead);
        set(a_transform.mounted, kMounted);
        if (a_transform.mounted && a_transform.flyingMount)
        {
            flags |= kFlyingMount;
            flags |= (static_cast<std::uint32_t>(a_transform.flyState) << kFlyStateShift) & kFlyStateMask;
            flags |= (static_cast<std::uint32_t>(a_transform.wingState) << kWingStateShift) & kWingStateMask;
        }
        set(a_transform.swimming, kSwimming);
        set(a_transform.levitating, kLevitating);
        set(a_transform.mistForm, kMistForm);
        set(a_transform.bats, kBats);
        set(a_transform.grounded, kGrounded);

        return QuantizedCheckpoint{ .sequence = a_transform.sequence,
            .x = Detail::RoundToInt32(a_transform.x * kPositionScale),
            .y = Detail::RoundToInt32(a_transform.y * kPositionScale),
            .z = Detail::RoundToInt32(a_transform.z * kPositionScale),
            .heading = static_cast<std::uint32_t>(std::lround(heading / kTwoPi * kHeadingSteps)) % kHeadingSteps,
            .pitch = Detail::QuantizePitch(a_transform.pitch),
            .lookPitch = Detail::QuantizePitch(a_transform.lookPitch),
            .speed = static_cast<std::uint32_t>(std::clamp(std::round(speed), 0.0F,
                static_cast<float>(kMaximumQuantizedSpeed))),
            .direction = static_cast<std::uint32_t>(std::lround(direction * kDirectionSteps)) % kDirectionSteps,
            .flags = flags,
            .time = a_transform.sampleTime % kSampleTimeModulus,
            .skeleton = a_transform.skeleton };
    }

    // Leaves out the skeleton pose: a received one may be a change from an earlier pose, which
    // the connection resolves (ServerClient).
    [[nodiscard]] inline bool IsValidQuantizedCheckpoint(const QuantizedCheckpoint& a_checkpoint) noexcept
    {
        using namespace CheckpointFlags;
        return a_checkpoint.heading < kHeadingSteps && Detail::IsValidQuantizedPitch(a_checkpoint.pitch)
            && Detail::IsValidQuantizedPitch(a_checkpoint.lookPitch) && a_checkpoint.speed <= kMaximumQuantizedSpeed
            && a_checkpoint.direction < kDirectionSteps && a_checkpoint.time < kSampleTimeModulus
            && (a_checkpoint.flags & ~kKnown) == 0
            && ((a_checkpoint.flags & kCastReleasedLeft) == 0 || (a_checkpoint.flags & kCastingLeft) != 0)
            && ((a_checkpoint.flags & kCastReleasedRight) == 0 || (a_checkpoint.flags & kCastingRight) != 0)
            && ((a_checkpoint.flags & kDualCasting) == 0 || (a_checkpoint.flags & kCastingLeft) != 0)
            && ((a_checkpoint.flags & (kShoutReleased | kShoutTwoWords | kShoutThreeWords)) == 0
                || (a_checkpoint.flags & kShouting) != 0)
            && (a_checkpoint.flags & (kShoutTwoWords | kShoutThreeWords)) != (kShoutTwoWords | kShoutThreeWords)
            && ((a_checkpoint.flags & kShoutFailed) == 0 || (a_checkpoint.flags & kShoutReleased) != 0)
            && ((a_checkpoint.flags & kFlyingMount) == 0 || (a_checkpoint.flags & kMounted) != 0)
            && ((a_checkpoint.flags & (kFlyStateMask | kWingStateMask)) == 0
                || (a_checkpoint.flags & kFlyingMount) != 0)
            && (a_checkpoint.flags & kFlyStateMask) >> kFlyStateShift
                   <= static_cast<std::uint32_t>(Multiplayer::kMaximumFlyState)
            && (a_checkpoint.flags & kMovementStateMask)
                   <= static_cast<std::uint32_t>(Multiplayer::MovementState::kSneakingRunning);
    }

    [[nodiscard]] inline PlayerTransform Dequantize(
        const QuantizedCheckpoint& a_checkpoint, const CheckpointContext& a_context)
    {
        using namespace CheckpointFlags;
        constexpr float kTwoPi = 6.28318530717958647692F;
        const auto flags = a_checkpoint.flags;
        return PlayerTransform{ .x = static_cast<float>(a_checkpoint.x) / kPositionScale,
            .y = static_cast<float>(a_checkpoint.y) / kPositionScale,
            .z = static_cast<float>(a_checkpoint.z) / kPositionScale,
            .heading = static_cast<float>(a_checkpoint.heading) * kTwoPi / static_cast<float>(kHeadingSteps),
            .pitch = static_cast<float>(a_checkpoint.pitch) / kPitchScale,
            .lookPitch = static_cast<float>(a_checkpoint.lookPitch) / kPitchScale,
            .speed = static_cast<float>(a_checkpoint.speed),
            .direction = static_cast<float>(a_checkpoint.direction) / static_cast<float>(kDirectionSteps),
            .animationState = Multiplayer::AnimationState{
                .movementState = static_cast<Multiplayer::MovementState>(flags & kMovementStateMask),
                .jumping = (flags & kJumping) != 0,
                .weaponDrawn = (flags & kWeaponDrawn) != 0 },
            .handPresentation = Multiplayer::HandPresentation{ .bowDrawn = (flags & kBowDrawn) != 0,
                .castingLeft = (flags & kCastingLeft) != 0,
                .castingRight = (flags & kCastingRight) != 0,
                .castReleasedLeft = (flags & kCastReleasedLeft) != 0,
                .castReleasedRight = (flags & kCastReleasedRight) != 0,
                .dualCasting = (flags & kDualCasting) != 0,
                .blocking = (flags & kBlocking) != 0 },
            .voice = Multiplayer::VoicePresentation{ .shouting = (flags & kShouting) != 0,
                .released = (flags & kShoutReleased) != 0,
                .failed = (flags & kShoutFailed) != 0,
                .words = static_cast<std::uint8_t>((flags & kShouting) == 0 ? 0
                        : (flags & kShoutThreeWords) != 0                ? 3
                        : (flags & kShoutTwoWords) != 0                  ? 2
                                                                         : 1) },
            .dead = (flags & kDead) != 0,
            .mounted = (flags & kMounted) != 0,
            .flyingMount = (flags & kFlyingMount) != 0,
            .flyState = static_cast<Multiplayer::FlyState>((flags & kFlyStateMask) >> kFlyStateShift),
            .wingState = static_cast<Multiplayer::WingState>((flags & kWingStateMask) >> kWingStateShift),
            .swimming = (flags & kSwimming) != 0,
            .levitating = (flags & kLevitating) != 0,
            .mistForm = (flags & kMistForm) != 0,
            .bats = (flags & kBats) != 0,
            .grounded = (flags & kGrounded) != 0,
            .interiorCell = a_context.interiorCell,
            .worldspace = a_context.worldspace,
            .furniture = a_context.furniture,
            .shapeshift = a_context.shapeshift,
            .idle = a_context.idle,
            .graphEpoch = a_context.graphEpoch,
            .sequence = a_checkpoint.sequence,
            .sampleTime = a_checkpoint.time,
            .skeleton = a_checkpoint.skeleton };
    }
}
