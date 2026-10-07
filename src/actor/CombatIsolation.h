#pragma once

#include <cstddef>

namespace CombatIsolation
{
    // Trampoline bytes InstallHooks needs: seven entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps.
    inline constexpr std::size_t kTrampolineSize = 4 * (5 + 2 * 14) + (6 + 2 * 14) + 2 * (7 + 2 * 14);

    void InstallHooks();
}
