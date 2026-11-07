#pragma once

#include <cstdint>

namespace Multiplayer
{
    // The values are Skyrim's FLY_STATE and the wire's FlyState values.
    enum class FlyState : std::uint8_t
    {
        kNone = 0,
        kTakeOff = 1,
        kCruising = 2,
        kHovering = 3,
        kLanding = 4,
        kPerching = 5,
        kAction = 6
    };

    inline constexpr FlyState kMaximumFlyState = FlyState::kAction;

    [[nodiscard]] constexpr const char* GetFlyStateName(FlyState a_state) noexcept
    {
        switch (a_state)
        {
            case FlyState::kNone:
                return "none";
            case FlyState::kTakeOff:
                return "takeOff";
            case FlyState::kCruising:
                return "cruising";
            case FlyState::kHovering:
                return "hovering";
            case FlyState::kLanding:
                return "landing";
            case FlyState::kPerching:
                return "perching";
            case FlyState::kAction:
                return "action";
        }
        return "unknown";
    }

    // The values are the wire's WingState values.
    enum class WingState : std::uint8_t
    {
        kNone = 0,
        kFlapping = 1,
        kGliding = 2,
        kFeathering = 3
    };

    inline constexpr WingState kMaximumWingState = WingState::kFeathering;

    [[nodiscard]] constexpr const char* GetWingStateName(WingState a_state) noexcept
    {
        switch (a_state)
        {
            case WingState::kNone:
                return "none";
            case WingState::kFlapping:
                return "flapping";
            case WingState::kGliding:
                return "gliding";
            case WingState::kFeathering:
                return "feathering";
        }
        return "unknown";
    }
}
