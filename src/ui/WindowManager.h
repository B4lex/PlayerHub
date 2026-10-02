#pragma once

#include "ui/Window.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace UI::WindowManager
{
    // Startup only, before the overlay and input hooks run. Windows draw in
    // registration order, so later ones appear on top.
    void Register(std::unique_ptr<Window> a_window);

    // Overlay thread, between ImGui::NewFrame and ImGui::Render.
    void DrawAll(float a_wheelSteps);

    // Safe to call from any thread.
    [[nodiscard]] bool HasFocus();

    // Whether the focused window, if any, uses the mouse. Safe to call from any
    // thread.
    [[nodiscard]] bool FocusedWindowUsesMouse();

    // The focused window's answers to the Window questions of the same names;
    // false and empty without one. Safe to call from any thread, except
    // GetFocusedCursorStart, which reads the window's layout: overlay thread.
    [[nodiscard]] bool FocusedWindowHidesIdleCursor();
    [[nodiscard]] bool FocusedWindowUsesTab();
    [[nodiscard]] std::optional<ScreenPoint> GetFocusedCursorStart();

    // Gives focus to the first window that uses a_scanCode as its focus key.
    // False if a window already has focus or none uses the key.
    [[nodiscard]] bool FocusByKey(std::uint32_t a_scanCode);

    // Releases focus if the window holding it uses a_scanCode as a release key.
    // False if no window has focus or the focused one does not use the key.
    [[nodiscard]] bool ReleaseFocusByKey(std::uint32_t a_scanCode);

    // Releases focus if a_window holds it. Safe to call from any thread.
    void ReleaseFocus(const Window& a_window);

    // Releases focus from whichever window holds it. Safe to call from any thread.
    void ReleaseFocus();
}
