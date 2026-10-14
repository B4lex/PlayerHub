#pragma once

#ifndef NDEBUG
#include <RE/Skyrim.h>

#include <cstdint>
#include <string>
#include <vector>

namespace GraphProbe
{
    struct Snapshot
    {
        const RE::hkbBehaviorGraph* graph{ nullptr };
        std::vector<std::uint32_t> words;
    };

    [[nodiscard]] std::string Describe(RE::Actor& a_actor, Snapshot& a_snapshot);
}
#endif
