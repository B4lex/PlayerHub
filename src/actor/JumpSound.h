#pragma once

#include <cstddef>

namespace JumpSound
{
    // Trampoline bytes InstallHooks needs: two call detours.
    inline constexpr std::size_t kTrampolineSize = 2 * 14;

    // Vanilla routes most jump footstep sounds through the local player's
    // first-person output model, which does not attenuate.
    void InstallHooks();
}
