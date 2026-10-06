#include "ui/WindowManager.h"

#include <atomic>
#include <utility>
#include <vector>

namespace UI::WindowManager
{
    namespace
    {
        std::vector<std::unique_ptr<Window>> s_windows;
        std::atomic<const Window*> s_focused{ nullptr };
    }

    void Register(std::unique_ptr<Window> a_window)
    {
        s_windows.push_back(std::move(a_window));
    }

    void DrawAll(float a_wheelSteps)
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        for (const auto& window : s_windows)
        {
            const bool hasFocus = window.get() == focused;
            window->Draw(WindowInput{ .focused = hasFocus, .wheelSteps = hasFocus ? a_wheelSteps : 0.0F });
        }
    }

    bool HasFocus()
    {
        return s_focused.load(std::memory_order_acquire) != nullptr;
    }

    bool FocusedWindowUsesMouse()
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        return focused && focused->UsesMouse();
    }

    bool FocusedWindowHidesIdleCursor()
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        return focused && focused->HidesIdleCursor();
    }

    bool FocusedWindowUsesTab()
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        return focused && focused->UsesTab();
    }

    std::optional<ScreenPoint> GetFocusedCursorStart()
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        return focused ? focused->GetCursorStart() : std::nullopt;
    }

    bool FocusByKey(std::uint32_t a_scanCode)
    {
        for (const auto& window : s_windows)
        {
            if (!window->IsFocusKey(a_scanCode))
            {
                continue;
            }
            const Window* expected = nullptr;
            return s_focused.compare_exchange_strong(
                expected, window.get(), std::memory_order_acq_rel, std::memory_order_acquire);
        }
        return false;
    }

    bool Focus(const Window& a_window)
    {
        const Window* expected = nullptr;
        return s_focused.compare_exchange_strong(
            expected, &a_window, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool ReleaseFocusByKey(std::uint32_t a_scanCode)
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        if (!focused || !focused->IsReleaseKey(a_scanCode))
        {
            return false;
        }
        ReleaseFocus(*focused);
        return true;
    }

    void ReleaseFocus(const Window& a_window)
    {
        const Window* expected = &a_window;
        s_focused.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void ReleaseFocus()
    {
        s_focused.store(nullptr, std::memory_order_release);
    }
}
