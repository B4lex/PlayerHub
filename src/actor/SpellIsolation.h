#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace SpellIsolation
{
    // Trampoline bytes InstallHooks needs: eight entry detours, sized by their
    // moved prologues (the destructible update's is longer on SE), and four call
    // detours.
    inline constexpr std::size_t kTrampolineSize = 6 * CodeDetour::EntryDetourTrampolineSize(5)
        + CodeDetour::EntryDetourTrampolineSize(7) + CodeDetour::EntryDetourTrampolineSize(8)
        + 4 * CodeDetour::kCallDetourTrampolineSize;

    void InstallHooks();
}
