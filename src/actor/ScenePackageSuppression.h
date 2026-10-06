#pragma once

#include <cstddef>

namespace ScenePackageSuppression
{
    // Trampoline bytes InstallHooks needs: two call detours.
    inline constexpr std::size_t kTrampolineSize = 2 * 14;

    void InstallHooks();
}
