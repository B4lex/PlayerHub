#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace UI::InputCapture
{
    inline constexpr std::size_t kTrampolineSize = CodeDetour::kCallDetourTrampolineSize;

    // Hooks the dispatch of Skyrim's input events. During gameplay, a window's
    // focus key gives it focus, and its release key gives focus back. While a
    // window has focus, the keyboard goes to Dear ImGui, and so does the mouse
    // if the window uses it; the game receives no keyboard or mouse input,
    // except the rest of presses it received before. Focus is released as soon
    // as a menu takes over input. The hook stays ahead of other plugins that
    // detour the same call, so they do not see a focused window's input either.
    // Requires kTrampolineSize trampoline bytes.
    void Install();

    // Overlay thread, after io.DisplaySize is set and before ImGui::NewFrame.
    // Forwards the input captured for the focused window to Dear ImGui, moving
    // a drawn cursor for a window that uses the mouse, and returns the mouse
    // wheel notches captured since the previous call for a keyboard-only one
    // or one whose cursor is still hidden.
    [[nodiscard]] float ForwardToImGui();
}
