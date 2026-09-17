#pragma once

namespace MainLoopHook
{
    // Detours Skyrim's main-loop update call. Requires 14 trampoline bytes.
    void Install();
}
