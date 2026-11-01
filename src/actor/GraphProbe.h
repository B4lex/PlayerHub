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

    struct TraceMachine
    {
        const RE::hkbStateMachine* machine{ nullptr };
        std::int32_t stateId{ 0 };
        const char* name{ nullptr };
        const char* stateName{ nullptr };
    };

    struct TraceVariable
    {
        const char* name{ nullptr };
        RE::hkbVariableInfo::VariableType type{};
        std::uint32_t word{ 0 };
    };

    // Names point into the graph's data: use them only in the pass that read them.
    struct TraceSample
    {
        const RE::hkbBehaviorGraph* graph{ nullptr };
        std::uint32_t activeGraph{ 0 };
        bool vanillaPrefix{ false };
        std::vector<TraceMachine> machines;
        std::vector<TraceVariable> variables;
        // Filled only by a read whose graph differs from the previous read's.
        std::vector<std::string> moddedEventNames;
    };

    [[nodiscard]] std::string Describe(RE::Actor& a_actor, Snapshot& a_snapshot);
    [[nodiscard]] std::string DescribeActiveClips(RE::Actor& a_actor);
    // Returns whether a graph was read.
    bool ReadTrace(RE::Actor& a_actor, TraceSample& a_sample);
}
#endif
