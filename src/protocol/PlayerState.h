#pragma once

#include "protocol/MovementState.h"

#include <cstdint>
#include <string>

namespace Network
{
    struct PlayerTransform
    {
        float x{ 0.0F };
        float y{ 0.0F };
        float z{ 0.0F };
        float heading{ 0.0F };
        // Measured translation speed in Skyrim world units per second.
        float speed{ 0.0F };
        // Travel direction relative to heading, in graph `Direction` units [0, 1).
        float direction{ 0.0F };
        // Locomotion, jump phase, and combat stance from the same snapshot.
        Multiplayer::AnimationState animationState{};
        // Bow draw, spell channelling, and blocking, gated on the receiver by weapon
        // stance.
        Multiplayer::HandPresentation handPresentation{};
        // The source is dying or dead. Receivers then leave its actor to a local
        // death and ragdoll, and ignore the presentation fields above.
        bool dead{ false };
        std::uint32_t cellID{ 0 };
        // Non-zero for exterior cells. Interior cells use zero and must match by
        // cell ID because their local coordinates are not globally comparable.
        std::uint32_t worldspaceID{ 0 };
        // Placed TESFurniture reference currently occupied by the source. Zero means
        // no furniture interaction. Form IDs require matching peer load orders.
        std::uint32_t furnitureRefID{ 0 };
        // Identifies the source animation-graph lifetime represented by this
        // checkpoint. Transient actions are only valid against the same epoch.
        std::uint64_t graphEpoch{ 1 };
        std::uint64_t sequence{ 0 };
    };

    // The attack vocabulary of the ordered action stream.
    enum class AttackAction : std::uint8_t
    {
        // A right-hand, or unarmed right, swing.
        kNormal,
        // A left-hand weapon or unarmed left swing.
        kLeft,
        // Both weapons, or both fists, at once.
        kDual,
        // A shield, weapon, torch, or bow bash.
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

    // The movement direction a power attack was started with. A sprinting power
    // attack is presented as a forward one.
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
        // A power attack, or a power bash, of the action's hand.
        bool power{ false };
        // Only meaningful for a power attack.
        PowerAttackDirection direction{ PowerAttackDirection::kStanding };
    };

    struct AnimationAction
    {
        std::string playerID;
        Attack attack{};
        std::uint64_t actionSequence{ 0 };
        std::uint64_t graphEpoch{ 0 };
        std::uint64_t checkpointSequence{ 0 };
    };
}
