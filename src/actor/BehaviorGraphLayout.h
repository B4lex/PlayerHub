#pragma once

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>

namespace BehaviorGraphLayout
{
    // Havok 2010 layouts that CommonLib keeps private or unnamed.
    // hkbStateMachine::StateInfo stores its name (hkStringPtr) at 0x60 and its
    // stateId at 0x68.
    inline constexpr std::size_t kStateNameOffset = 0x60;
    inline constexpr std::size_t kStateIdOffset = 0x68;
    static_assert(sizeof(RE::hkbStateMachine::StateInfo) > kStateIdOffset);

    template <class T>
    const T& ReadAt(const void* a_object, std::size_t a_offset)
    {
        return *reinterpret_cast<const T*>(static_cast<const std::byte*>(a_object) + a_offset);
    }

    // Not hkStringPtr::c_str(): CommonLib masks the managed bit with ~kManaged, and
    // MSVC zero-extends that 32-bit enum mask, so a name above 4 GiB lost its high bits.
    inline const char* GetName(const RE::hkStringPtr& a_name)
    {
        const auto raw = ReadAt<std::uintptr_t>(&a_name, 0);
        const auto* name = reinterpret_cast<const char*>(raw & ~std::uintptr_t{ 1 });
        return name ? name : "?";
    }

    inline const char* GetCurrentStateName(const RE::hkbStateMachine& a_machine)
    {
        for (const auto* stateInfo : a_machine.states)
        {
            if (stateInfo && ReadAt<std::int32_t>(stateInfo, kStateIdOffset) == a_machine.currentStateID)
            {
                return GetName(ReadAt<RE::hkStringPtr>(stateInfo, kStateNameOffset));
            }
        }
        return "?";
    }

    // The caller holds the manager's updateLock.
    inline const RE::hkbBehaviorGraph* GetActiveBehavior(RE::BSAnimationGraphManager& a_manager)
    {
        const auto activeGraph = a_manager.GetRuntimeData().activeGraph;
        const auto* graph = activeGraph < a_manager.graphs.size() ? a_manager.graphs[activeGraph].get() : nullptr;
        return graph ? graph->behaviorGraph : nullptr;
    }
}
