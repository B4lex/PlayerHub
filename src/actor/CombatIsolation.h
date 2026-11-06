#pragma once

#include <cstddef>

namespace CombatIsolation
{
    // Trampoline bytes InstallHooks needs: six entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps. 1.7.104 has the longer
    // prologues.
    inline constexpr std::size_t kTrampolineSize = 3 * (5 + 2 * 14) + (6 + 2 * 14) + 2 * (7 + 2 * 14);

    void InstallHooks();
}
