#pragma once

#include <cstddef>

namespace UI::Overlay
{
    // Trampoline bytes Install needs: one 5-byte call detour.
    inline constexpr std::size_t kTrampolineSize = 14;

    // Hooks the end of Skyrim's frame render, just before the swap chain
    // presents, and draws the Dear ImGui overlay there. The ImGui context is
    // created lazily on the first frame that has a device and swap chain.
    // Requires kTrampolineSize trampoline bytes.
    void Install();
}
