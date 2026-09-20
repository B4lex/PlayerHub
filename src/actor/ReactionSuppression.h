#pragma once

#include <cstddef>

namespace ReactionSuppression
{
    // Trampoline bytes InstallHooks needs: three entry detours, each a gateway of
    // the moved prologue plus two 14-byte absolute jumps.
    inline constexpr std::size_t kTrampolineSize = (7 + 2 * 14) + 2 * (6 + 2 * 14);

    // Stops a remote actor's own AI from reacting to the local world: it starts
    // no dialogue responses (the barks Skyrim picks for it, such as hit lines),
    // never reports fleeing, and records no detection of other actors. Local
    // actors still detect and greet it. Requires kTrampolineSize trampoline bytes.
    void InstallHooks();
}
