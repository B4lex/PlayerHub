#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace SpellIsolation
{
    // Call sites InstallHooks detours on the runtime with the most of them (AE).
    inline constexpr std::size_t kCallDetourCount = 16;
    // Trampoline bytes InstallHooks needs: two entry detours with 5-byte
    // prologues and the call detours.
    inline constexpr std::size_t kTrampolineSize
        = 2 * CodeDetour::EntryDetourTrampolineSize(5) + kCallDetourCount * CodeDetour::kCallDetourTrampolineSize;

    void InstallHooks();
    [[nodiscard]] bool IsolatesNativeCasts();
}
