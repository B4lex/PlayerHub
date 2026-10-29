#include "ui/WindowManager.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace UI::WindowManager
{
    namespace
    {
        std::vector<std::unique_ptr<Window>> s_windows;
        // Written under s_focusLock, which keeps the two consistent; s_focused is also
        // read without it on every input event and draw.
        std::mutex s_focusLock;
        std::atomic<const Window*> s_focused{ nullptr };
        // The window a borrow key took focus from. Never set without s_focused.
        std::atomic<const Window*> s_lender{ nullptr };

        // The lender first, so a reader that sees the new focus also sees its lender.
        void Publish(const Window* a_focused, const Window* a_lender)
        {
            s_lender.store(a_lender, std::memory_order_release);
            s_focused.store(a_focused, std::memory_order_release);
        }

        void Release(const Window& a_window)
        {
            if (s_focused.load(std::memory_order_acquire) == &a_window)
            {
                Publish(s_lender.load(std::memory_order_acquire), nullptr);
            }
            else if (s_lender.load(std::memory_order_acquire) == &a_window)
            {
                s_lender.store(nullptr, std::memory_order_release);
            }
        }
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
        const std::scoped_lock lock{ s_focusLock };
        const auto* focused = s_focused.load(std::memory_order_acquire);
        // The lender's cursor stays where it is.
        return focused && focused->HidesIdleCursor() && !s_lender.load(std::memory_order_acquire);
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
            const std::scoped_lock lock{ s_focusLock };
            const auto* focused = s_focused.load(std::memory_order_acquire);
            if (focused && !(focused->LendsFocus() && window->IsBorrowKey(a_scanCode)))
            {
                return false;
            }
            // Before focus is published, so the window never draws focused without its key.
            window->OnFocusKey(a_scanCode);
            Publish(window.get(), focused);
            return true;
        }
        return false;
    }

    bool TakeFocus(const Window& a_window)
    {
        const std::scoped_lock lock{ s_focusLock };
        Publish(&a_window, nullptr);
        return true;
    }

    bool Focus(const Window& a_window)
    {
        const std::scoped_lock lock{ s_focusLock };
        if (s_focused.load(std::memory_order_acquire))
        {
            return false;
        }
        Publish(&a_window, nullptr);
        return true;
    }

    bool Borrow(const Window& a_borrower)
    {
        const std::scoped_lock lock{ s_focusLock };
        const auto* focused = s_focused.load(std::memory_order_acquire);
        if (!focused || focused == &a_borrower || !focused->LendsFocus())
        {
            return false;
        }
        Publish(&a_borrower, focused);
        return true;
    }

    bool HoldsFocus(const Window& a_window)
    {
        const std::scoped_lock lock{ s_focusLock };
        return s_focused.load(std::memory_order_acquire) == &a_window
            || s_lender.load(std::memory_order_acquire) == &a_window;
    }

    bool ReleaseFocusByKey(std::uint32_t a_scanCode)
    {
        const std::scoped_lock lock{ s_focusLock };
        const auto* focused = s_focused.load(std::memory_order_acquire);
        if (!focused || !focused->IsReleaseKey(a_scanCode))
        {
            return false;
        }
        Release(*focused);
        return true;
    }

    bool CaptureKey(std::uint32_t a_scanCode)
    {
        const auto* focused = s_focused.load(std::memory_order_acquire);
        if (!focused || !focused->CapturesKeys())
        {
            return false;
        }
        for (const auto& window : s_windows)
        {
            if (window.get() == focused)
            {
                window->OnCapturedKey(a_scanCode);
                return true;
            }
        }
        return false;
    }

    void ReleaseFocus(const Window& a_window)
    {
        const std::scoped_lock lock{ s_focusLock };
        Release(a_window);
    }

    void ReleaseFocus()
    {
        const std::scoped_lock lock{ s_focusLock };
        Publish(nullptr, nullptr);
    }
}
