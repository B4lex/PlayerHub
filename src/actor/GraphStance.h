#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>

namespace GraphStance
{
    // kOff is not sneaking, standing, not sprinting, on the ground, or not in an idle;
    // kOn the opposite.
    enum class SelectorState
    {
        kNone,
        kOff,
        kOn,
        kMixed
    };

    struct StanceSelectors
    {
        SelectorState sneak{ SelectorState::kNone };
        SelectorState moving{ SelectorState::kNone };
        SelectorState sprinting{ SelectorState::kNone };
        SelectorState levitating{ SelectorState::kNone };
        // kOn while any idle machine is in an idle, never kMixed: the root machines
        // stay in their default state while a nested one plays an idle.
        SelectorState idle{ SelectorState::kNone };
        // The idle is the weapon-drawn combat idle (1HM_Behavior's CombatIdles).
        bool drawnIdle{ false };
    };

    [[nodiscard]] StanceSelectors ReadStanceSelectors(RE::Actor& a_actor);
    [[nodiscard]] std::string DescribeStanceSelectors(RE::Actor& a_actor);
    [[nodiscard]] const char* GetMovingName(SelectorState a_selectors) noexcept;

    enum class Sneak
    {
        kUnknown,
        kNotSneaking,
        kSneaking
    };

    struct SneakReading
    {
        Sneak sneak{ Sneak::kUnknown };
        // Selector states, when any is active; otherwise the sneak variables.
        SelectorState selectors{ SelectorState::kNone };
    };

    // Falls back to the sneak variables when no sneak selector is active.
    [[nodiscard]] SneakReading ReadSneak(const RE::Actor& a_actor, SelectorState a_selectors);
    [[nodiscard]] const char* GetSneakName(const SneakReading& a_reading) noexcept;

    // The graph's iState, or -1 without one.
    [[nodiscard]] std::int32_t ReadStateTag(const RE::Actor& a_actor);
}
