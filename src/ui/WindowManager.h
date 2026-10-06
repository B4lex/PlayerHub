#pragma once

#include "ui/Window.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace UI::WindowManager
{
    void Register(std::unique_ptr<Window> a_window);

    void DrawAll(float a_wheelSteps);

    [[nodiscard]] bool HasFocus();

    [[nodiscard]] bool FocusedWindowUsesMouse();

    [[nodiscard]] bool FocusedWindowHidesIdleCursor();
    [[nodiscard]] bool FocusedWindowUsesTab();
    [[nodiscard]] std::optional<ScreenPoint> GetFocusedCursorStart();

    [[nodiscard]] bool FocusByKey(std::uint32_t a_scanCode);

    [[nodiscard]] bool Focus(const Window& a_window);

    // Takes focus from any other window, as a window the player must see now does.
    bool TakeFocus(const Window& a_window);

    [[nodiscard]] bool ReleaseFocusByKey(std::uint32_t a_scanCode);

    void ReleaseFocus(const Window& a_window);

    void ReleaseFocus();
}
