#pragma once

#include <cstdint>

namespace Multiplayer
{
    enum class Shapeshift : std::uint8_t
    {
        kNone,
        kWerewolf,
        kVampireLord,
        kWerebear
    };

    inline constexpr Shapeshift kMaximumShapeshift = Shapeshift::kWerebear;

    [[nodiscard]] constexpr const char* GetShapeshiftName(Shapeshift a_form) noexcept
    {
        switch (a_form)
        {
            case Shapeshift::kNone:
                return "none";
            case Shapeshift::kWerewolf:
                return "werewolf";
            case Shapeshift::kVampireLord:
                return "vampireLord";
            case Shapeshift::kWerebear:
                return "werebear";
        }
        return "unknown";
    }

    // The werebear race uses the werewolf's behavior graph.
    [[nodiscard]] constexpr Shapeshift GetBeastGraph(Shapeshift a_form) noexcept
    {
        return a_form == Shapeshift::kWerebear ? Shapeshift::kWerewolf : a_form;
    }

    // The values are the wire's BeastAction values.
    enum class BeastAction : std::uint8_t
    {
        kNone = 0,
        kWerewolfAttackRight = 1,
        kWerewolfAttackLeft,
        kWerewolfAttackRightPower,
        kWerewolfAttackLeftPower,
        kWerewolfAttackDual,
        kWerewolfAttackBackhand,
        kWerewolfAttackRightSide,
        kWerewolfAttackLeftSide,
        kWerewolfAttackRightRunningPower,
        kWerewolfAttackLeftRunningPower,
        kWerewolfAttackDualRunning,
        kWerewolfAttackDualSprinting,
        kWerewolfAttackRightSprinting,
        kWerewolfAttackLeftSprinting,
        kWerewolfHowlStart,
        kWerewolfHowlRelease,
        kWerewolfHowlEnd,
        kWerewolfFeed,
        kWerewolfFeedPaired,
        kVampireLordAttackRight = 32,
        kVampireLordAttackLeft,
        kVampireLordAttackPowerRight,
        kVampireLordAttackPowerLeft,
        kVampireLordBatsStart,
        kVampireLordBatsStop,
        kVampireLordFeedFront,
        kVampireLordFeedBack,
        kVampireLordCastBeginLeft,
        kVampireLordCastBeginRight,
        kVampireLordCastAimedLeft,
        kVampireLordCastAimedRight,
        kVampireLordCastConcentrationLeft,
        kVampireLordCastConcentrationRight,
        kVampireLordCastReadyLeft,
        kVampireLordCastReadyRight,
        kVampireLordCastReleaseLeft,
        kVampireLordCastReleaseRight,
        kVampireLordCastInterruptLeft,
        kVampireLordCastInterruptRight
    };

    // The graph that plays the action, kNone for a value outside the vocabulary.
    [[nodiscard]] constexpr Shapeshift GetBeastActionGraph(std::uint32_t a_value) noexcept
    {
        if (a_value >= static_cast<std::uint32_t>(BeastAction::kWerewolfAttackRight)
            && a_value <= static_cast<std::uint32_t>(BeastAction::kWerewolfFeedPaired))
        {
            return Shapeshift::kWerewolf;
        }
        if (a_value >= static_cast<std::uint32_t>(BeastAction::kVampireLordAttackRight)
            && a_value <= static_cast<std::uint32_t>(BeastAction::kVampireLordCastInterruptRight))
        {
            return Shapeshift::kVampireLord;
        }
        return Shapeshift::kNone;
    }

    [[nodiscard]] constexpr Shapeshift GetBeastActionGraph(BeastAction a_action) noexcept
    {
        return GetBeastActionGraph(static_cast<std::uint32_t>(a_action));
    }

    [[nodiscard]] constexpr bool IsBeastAttack(BeastAction a_action) noexcept
    {
        return (a_action >= BeastAction::kWerewolfAttackRight && a_action <= BeastAction::kWerewolfAttackLeftSprinting)
            || (a_action >= BeastAction::kVampireLordAttackRight
                && a_action <= BeastAction::kVampireLordAttackPowerLeft);
    }

    static_assert(GetBeastActionGraph(BeastAction::kWerewolfFeedPaired) == Shapeshift::kWerewolf);
    static_assert(GetBeastActionGraph(20U) == Shapeshift::kNone);
    static_assert(GetBeastActionGraph(BeastAction::kVampireLordCastInterruptRight) == Shapeshift::kVampireLord);
    static_assert(static_cast<int>(BeastAction::kVampireLordCastInterruptRight) == 51);
    static_assert(GetBeastActionGraph(BeastAction::kNone) == Shapeshift::kNone);
    static_assert(
        IsBeastAttack(BeastAction::kWerewolfAttackLeftSprinting) && !IsBeastAttack(BeastAction::kWerewolfHowlStart));
    static_assert(
        IsBeastAttack(BeastAction::kVampireLordAttackPowerLeft) && !IsBeastAttack(BeastAction::kVampireLordBatsStart));
}
