#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace SpellIsolation
{
    // Trampoline bytes InstallHooks needs: nine entry detours, sized by their
    // moved prologues (the destructible update's is longer on SE, the impact
    // effects' on 1.7.104), and one call detour.
    inline constexpr std::size_t kTrampolineSize = 6 * CodeDetour::EntryDetourTrampolineSize(5)
        + 2 * CodeDetour::EntryDetourTrampolineSize(7) + CodeDetour::EntryDetourTrampolineSize(8)
        + CodeDetour::kCallDetourTrampolineSize;

    void InstallHooks();
}
