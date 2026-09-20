#pragma once

#include <cstddef>

namespace ScenePackageSuppression
{
    // Trampoline bytes InstallHooks needs: two call detours.
    inline constexpr std::size_t kTrampolineSize = 2 * 14;

    // Keeps local scenes from running package actions on a remote actor filled
    // into one of their quest aliases, such as the bard audience sent to dance in
    // front of a singing bard. Requires kTrampolineSize trampoline bytes.
    void InstallHooks();
}
