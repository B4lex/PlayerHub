#include "actor/ModdedGraph.h"
#include "actor/BehaviorGraphLayout.h"
#include "actor/GraphVocabulary.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <string_view>

namespace ModdedGraph
{
    namespace
    {
        using namespace BehaviorGraphLayout;
        using GraphVocabulary::kVanillaEventCount;

        // BSAnimationGraphManager keeps the third-person graph first.
        constexpr std::uint32_t kThirdPersonGraph = 0;

        // The graph data a read needs, valid while the manager's updateLock is held.
        struct GraphView
        {
            const RE::hkbBehaviorGraph* behavior{ nullptr };
            const RE::hkbBehaviorGraphData* data{ nullptr };
            const RE::hkbBehaviorGraphStringData* strings{ nullptr };
            std::uint32_t activeGraph{ 0 };
        };

        template <class Visitor>
        bool WithGraph(RE::IAnimationGraphManagerHolder& a_holder, Visitor&& a_visitor)
        {
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
            if (!a_holder.GetAnimationGraphManager(manager) || !manager)
            {
                return false;
            }
            RE::BSSpinLockGuard lock{ manager->GetRuntimeData().updateLock };
            if (manager->graphs.size() <= kThirdPersonGraph || !manager->graphs[kThirdPersonGraph])
            {
                return false;
            }
            GraphView view{ .behavior = manager->graphs[kThirdPersonGraph]->behaviorGraph,
                .activeGraph = manager->GetRuntimeData().activeGraph };
            view.data = view.behavior ? view.behavior->data.get() : nullptr;
            view.strings = view.data ? view.data->stringData.get() : nullptr;
            if (!view.strings)
            {
                return false;
            }
            a_visitor(view);
            return true;
        }

        RE::IAnimationGraphManagerHolder& AsHolder(RE::Actor& a_actor)
        {
            return static_cast<RE::IAnimationGraphManagerHolder&>(a_actor);
        }

        // An exact vtable match instead of an RTTI cast per active node, as in GraphStance.
        template <class Visitor>
        void ForEachActiveMachine(const RE::hkbBehaviorGraph& a_behavior, Visitor&& a_visitor)
        {
            if (!a_behavior.activeNodes)
            {
                return;
            }
            static const REL::Relocation<std::uintptr_t> stateMachineVTable{ RE::VTABLE_hkbStateMachine[0] };
            for (const auto& nodeInfo : *a_behavior.activeNodes)
            {
                const auto* node = nodeInfo.nodeClone ? nodeInfo.nodeClone : nodeInfo.nodeTemplate;
                if (node && ReadAt<std::uintptr_t>(node, 0) == stateMachineVTable.address())
                {
                    a_visitor(*static_cast<const RE::hkbStateMachine*>(node));
                }
            }
        }

        // The machine's name hash when it reads as modded, otherwise 0. GetCurrentStateName
        // reads "?" for a state it cannot find, which counts as vanilla.
        std::uint64_t GetModdedMachine(const RE::hkbStateMachine& a_machine, const char*& a_state)
        {
            const auto machine = GraphVocabulary::HashName(GetName(a_machine.name));
            a_state = GetCurrentStateName(a_machine);
            if (!GraphVocabulary::IsVanillaMachine(machine))
            {
                return machine;
            }
            const std::string_view state{ a_state };
            return state != "?" && !GraphVocabulary::IsVanillaState(GraphVocabulary::HashName(state)) ? machine : 0;
        }
    }

    Layout ReadLayout(RE::Actor& a_actor)
    {
        Layout layout;
        WithGraph(AsHolder(a_actor), [&](const GraphView& a_view) {
            layout.behavior = a_view.behavior;
            layout.activeGraph = a_view.activeGraph;
            layout.vanillaPrefix = GraphVocabulary::HasVanillaPrefix(*a_view.strings);
            if (layout.vanillaPrefix)
            {
                layout.appendedEvents = a_view.strings->eventNames.size() - kVanillaEventCount;
            }
        });
        return layout;
    }

    StateReading ReadModdedState(RE::Actor& a_actor, std::span<const std::uint64_t> a_ignored)
    {
        PROFILE_FUNCTION();
        StateReading reading;
        WithGraph(AsHolder(a_actor), [&](const GraphView& a_view) {
            reading.available = a_view.behavior->activeNodes != nullptr;
            ForEachActiveMachine(*a_view.behavior, [&](const RE::hkbStateMachine& a_machine) {
                if (reading.modded)
                {
                    return;
                }
                const char* state = nullptr;
                const auto machine = GetModdedMachine(a_machine, state);
                if (machine != 0 && std::ranges::find(a_ignored, machine) == a_ignored.end())
                {
                    reading.modded = true;
                    reading.machine = GetName(a_machine.name);
                    reading.state = state;
                }
            });
        });
        return reading;
    }

    void CollectModdedMachines(RE::Actor& a_actor, std::vector<std::uint64_t>& a_hashes)
    {
        a_hashes.clear();
        WithGraph(AsHolder(a_actor), [&](const GraphView& a_view) {
            ForEachActiveMachine(*a_view.behavior, [&](const RE::hkbStateMachine& a_machine) {
                const char* state = nullptr;
                if (const auto machine = GetModdedMachine(a_machine, state); machine != 0)
                {
                    a_hashes.push_back(machine);
                }
            });
        });
    }
}
