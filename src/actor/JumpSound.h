#pragma once

#include <cstddef>

namespace JumpSound
{
    // Trampoline bytes InstallHooks needs: two call detours.
    inline constexpr std::size_t kTrampolineSize = 2 * 14;

    // Fades a remote actor's takeoff and landing footstep sounds with distance.
    // Vanilla routes most of them through the local player's first-person output
    // model, which does not attenuate, so every remote jump would be heard at
    // full volume anywhere. Requires kTrampolineSize trampoline bytes.
    void InstallHooks();
}
