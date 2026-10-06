#pragma once

#include "protocol/FormReference.h"
#include "protocol/MovementState.h"
#include "protocol/PeerSlot.h"

#include <cstdint>

namespace Network
{
    // Skyrim clamps a controller's pitch well inside this bound.
    inline constexpr float kMaximumPitch = 1.57079632679489661923F;

    struct PlayerTransform
    {
        float x{ 0.0F };
        float y{ 0.0F };
        float z{ 0.0F };
        float heading{ 0.0F };
        // Skyrim eases a pitching race's controller toward the ground's slope and
        // keeps any other at zero.
        float pitch{ 0.0F };
        float speed{ 0.0F };
        float direction{ 0.0F };
        Multiplayer::AnimationState animationState{};
        Multiplayer::HandPresentation handPresentation{};
        bool dead{ false };
        bool mounted{ false };
        bool swimming{ false };
        std::uint32_t cellID{ 0 };
        std::uint32_t worldspaceID{ 0 };
        Multiplayer::FormReference furniture;
        std::uint64_t graphEpoch{ 1 };
        std::uint64_t sequence{ 0 };
        std::uint32_t sampleTime{ 0 };
    };

    enum class AttackAction : std::uint8_t
    {
        kNormal,
        kLeft,
        kDual,
        kBash
    };

    [[nodiscard]] constexpr const char* GetAttackActionName(AttackAction a_action) noexcept
    {
        switch (a_action)
        {
            case AttackAction::kLeft:
                return "left";
            case AttackAction::kDual:
                return "dual";
            case AttackAction::kBash:
                return "bash";
            default:
                return "normal";
        }
    }

    enum class PowerAttackDirection : std::uint8_t
    {
        kStanding,
        kForward,
        kBackward,
        kLeft,
        kRight
    };

    [[nodiscard]] constexpr const char* GetPowerAttackDirectionName(PowerAttackDirection a_direction) noexcept
    {
        switch (a_direction)
        {
            case PowerAttackDirection::kForward:
                return "forward";
            case PowerAttackDirection::kBackward:
                return "backward";
            case PowerAttackDirection::kLeft:
                return "left";
            case PowerAttackDirection::kRight:
                return "right";
            default:
                return "standing";
        }
    }

    struct Attack
    {
        AttackAction action{ AttackAction::kNormal };
        bool power{ false };
        PowerAttackDirection direction{ PowerAttackDirection::kStanding };
    };

    struct AnimationAction
    {
        Multiplayer::PeerSlot playerID{ 0 };
        Attack attack{};
        std::uint64_t actionSequence{ 0 };
        std::uint64_t graphEpoch{ 0 };
        std::uint64_t checkpointSequence{ 0 };
    };
}
