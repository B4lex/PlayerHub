#pragma once

#include <cstddef>

namespace CombatIsolation
{
    // Trampoline bytes InstallHooks needs: five entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps, and three call detours.
    inline constexpr std::size_t kTrampolineSize = 4 * (5 + 2 * 14) + (6 + 2 * 14) + 3 * 14;

    void InstallHooks();
}
