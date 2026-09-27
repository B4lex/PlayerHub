#pragma once

#include "ui/InputCapture.h"

#include <cstddef>

namespace UI::Overlay
{
    // Trampoline bytes Install needs: the renderer's 5-byte call detour and the
    // input capture hook.
    inline constexpr std::size_t kTrampolineSize = 14 + InputCapture::kTrampolineSize;

    // Registers the overlay windows, installs input capture, and hooks the end
    // of Skyrim's frame render, just before the swap chain presents, to draw the
    // Dear ImGui overlay there. The ImGui context is created lazily on the first
    // frame that has a device and swap chain. Requires kTrampolineSize
    // trampoline bytes.
    void Install();
}
