#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace UI::InputCapture
{
    inline constexpr std::size_t kTrampolineSize = CodeDetour::kCallDetourTrampolineSize;

    // Hooks the dispatch of Skyrim's input events. During gameplay, a window's
    // focus key gives it focus. While a window has focus, the keyboard goes to
    // Dear ImGui and the game receives no keyboard or mouse input, except the
    // rest of presses it received before. Focus is released as soon as a menu
    // takes over input. Requires kTrampolineSize trampoline bytes.
    void Install();

    // Overlay thread, before ImGui::NewFrame. Forwards the input captured for
    // the focused window to Dear ImGui and returns the mouse wheel notches
    // captured since the previous call.
    [[nodiscard]] float ForwardToImGui();
}
