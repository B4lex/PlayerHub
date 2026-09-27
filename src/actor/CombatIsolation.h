#pragma once

#include <cstddef>

namespace CombatIsolation
{
    // Trampoline bytes InstallHooks needs: five entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps, and three call detours.
    inline constexpr std::size_t kTrampolineSize = 4 * (5 + 2 * 14) + (6 + 2 * 14) + 3 * 14;

    // Keeps local actors from treating a remote actor as hostile or starting
    // combat with it, and vice versa, drops the melee and projectile hits a
    // remote actor's replicated attacks would land on the local world and the
    // melee hits local actors would land on it, and keeps its bow draws and casts
    // from releasing arrows or spells. Requires kTrampolineSize trampoline bytes.
    void InstallHooks();
}
