#pragma once

#include <cstddef>

namespace RemoteCollision
{
    // Trampoline bytes InstallHooks needs: one call detour.
    inline constexpr std::size_t kTrampolineSize = 14;

    // Lets characters pass through remote actors, and remote actors through other
    // characters, while every controller keeps its world collision. Requires
    // kTrampolineSize trampoline bytes.
    void InstallHooks();
}
