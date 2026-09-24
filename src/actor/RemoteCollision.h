#pragma once

#include <RE/Skyrim.h>

#include <cstddef>

namespace RemoteCollision
{
    // Trampoline bytes InstallHooks needs: one call detour.
    inline constexpr std::size_t kTrampolineSize = 14;

    // Lets characters pass through remote actors, and remote actors through other
    // characters, while every controller keeps its world collision. Requires
    // kTrampolineSize trampoline bytes.
    void InstallHooks();

    // Takes the actor's own character body out of, or back into, every character
    // collision; its controller keeps its world collision. Returns whether the
    // body changed. A rebuilt controller starts colliding again, so a caller
    // reasserts it while it applies. Main-loop thread only.
    bool SetCharacterBodyCollision(RE::Actor& a_actor, bool a_enabled);
}
