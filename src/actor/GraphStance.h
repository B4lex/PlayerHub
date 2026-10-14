#pragma once

#include <RE/Skyrim.h>

#include <string>

namespace GraphStance
{
    // kOff is not sneaking, standing, or not sprinting; kOn the opposite.
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
    };

    [[nodiscard]] StanceSelectors ReadStanceSelectors(RE::Actor& a_actor);
    [[nodiscard]] std::string DescribeStanceSelectors(RE::Actor& a_actor);
}
