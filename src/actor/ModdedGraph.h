#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <span>
#include <vector>

// Reads of the local player's third-person behavior graph for modded behavior: the graph
// whose skeleton peers receive. Callers hold no graph lock.
namespace ModdedGraph
{
    struct Layout
    {
        // Compared only, never dereferenced.
        const RE::hkbBehaviorGraph* behavior{ nullptr };
        bool vanillaPrefix{ false };
        std::uint32_t activeGraph{ 0 };
        std::int32_t moddedEvents{ 0 };
    };

    [[nodiscard]] Layout ReadLayout(RE::Actor& a_actor);
    // Sorted GraphVocabulary::HashName values of the modded event names; empty unless
    // the graph is a_expected and has the vanilla prefix.
    void CollectModdedEvents(
        RE::Actor& a_actor, const RE::hkbBehaviorGraph* a_expected, std::vector<std::uint64_t>& a_hashes);

    // Names point into the graph's data: use them only in the pass that read them.
    struct StateReading
    {
        bool available{ false };
        bool modded{ false };
        const char* machine{ nullptr };
        const char* state{ nullptr };
    };

    // Modded: an active state machine whose name is not vanilla, or a vanilla machine in
    // a state whose name is not vanilla. Machines whose name hash a_ignored holds count
    // as vanilla.
    [[nodiscard]] StateReading ReadModdedState(RE::Actor& a_actor, std::span<const std::uint64_t> a_ignored);
    // The name hashes of the machines ReadModdedState counts as modded, unsorted.
    void CollectModdedMachines(RE::Actor& a_actor, std::vector<std::uint64_t>& a_hashes);
}
