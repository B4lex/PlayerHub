#pragma once

#include <cmath>
#include <cstdint>

namespace Multiplayer
{
    enum class MovementState : std::uint8_t
    {
        kIdle,
        kWalking,
        kRunning,
        kSprinting,
        kSneakingIdle,
        kSneakingWalking,
        kSneakingRunning
    };

    [[nodiscard]] constexpr bool IsMoving(MovementState a_state) noexcept
    {
        return a_state != MovementState::kIdle && a_state != MovementState::kSneakingIdle;
    }

    [[nodiscard]] constexpr bool IsSneaking(MovementState a_state) noexcept
    {
        return a_state == MovementState::kSneakingIdle || a_state == MovementState::kSneakingWalking
            || a_state == MovementState::kSneakingRunning;
    }

    [[nodiscard]] constexpr bool IsSprinting(MovementState a_state) noexcept
    {
        return a_state == MovementState::kSprinting;
    }

    struct AnimationState
    {
        MovementState movementState{ MovementState::kIdle };
        bool jumping{ false };
        bool weaponDrawn{ false };

        constexpr bool operator==(const AnimationState&) const noexcept = default;
    };

    struct HandPresentation
    {
        bool bowDrawn{ false };
        bool castingLeft{ false };
        bool castingRight{ false };
        bool blocking{ false };

        [[nodiscard]] constexpr bool IsCasting(bool a_leftHand) const noexcept
        {
            return a_leftHand ? castingLeft : castingRight;
        }

        constexpr bool operator==(const HandPresentation&) const noexcept = default;
    };

    // Movement direction relative to the body facing, in the behavior graph's
    // `Direction` units: 0 forward, 0.25 right, 0.5 backward, 0.75 left.
    [[nodiscard]] inline float NormalizeMovementDirection(float a_direction) noexcept
    {
        if (!std::isfinite(a_direction))
        {
            return 0.0F;
        }
        float direction = a_direction - std::floor(a_direction);
        return direction >= 1.0F ? 0.0F : direction;
    }

    [[nodiscard]] constexpr const char* GetMovementStateName(MovementState a_state) noexcept
    {
        switch (a_state)
        {
            case MovementState::kIdle:
                return "idle";
            case MovementState::kWalking:
                return "walking";
            case MovementState::kRunning:
                return "running";
            case MovementState::kSprinting:
                return "sprinting";
            case MovementState::kSneakingIdle:
                return "sneakingIdle";
            case MovementState::kSneakingWalking:
                return "sneakingWalking";
            case MovementState::kSneakingRunning:
                return "sneakingRunning";
        }

        return "unknown";
    }
}
