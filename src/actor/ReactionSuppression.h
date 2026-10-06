#pragma once

#include <cstddef>

namespace ReactionSuppression
{
    // Trampoline bytes InstallHooks needs: three entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps.
    inline constexpr std::size_t kTrampolineSize = (7 + 2 * 14) + 2 * (6 + 2 * 14);

    void InstallHooks();
}
