#pragma once

#include <cstdint>
#include <optional>

namespace UI
{
    struct WindowInput
    {
        bool focused{ false };
        float wheelSteps{ 0.0F };
    };

    struct ScreenPoint
    {
        float x{ 0.0F };
        float y{ 0.0F };
    };

    class Window
    {
    public:
        virtual ~Window() = default;

        [[nodiscard]] virtual bool IsFocusKey(std::uint32_t) const { return false; }

        // Called on the input thread when one of the window's focus keys is about to
        // give it focus, before Draw can see the focus.
        virtual void OnFocusKey(std::uint32_t) {}

        // Called on the input thread, and by WindowManager::Borrow on the overlay thread, while
        // the window holds focus: whether another window may take focus from it now.
        [[nodiscard]] virtual bool LendsFocus() const { return false; }

        // One of the window's focus keys that also takes focus from a window that lends it.
        [[nodiscard]] virtual bool IsBorrowKey(std::uint32_t) const { return false; }

        [[nodiscard]] virtual bool IsReleaseKey(std::uint32_t) const { return false; }

        [[nodiscard]] virtual bool CapturesKeys() const { return false; }

        // Called on the input thread for each key pressed while CapturesKeys is true.
        virtual void OnCapturedKey(std::uint32_t) {}

        [[nodiscard]] virtual bool UsesMouse() const { return false; }

        [[nodiscard]] virtual bool HidesIdleCursor() const { return false; }

        [[nodiscard]] virtual std::optional<ScreenPoint> GetCursorStart() const { return std::nullopt; }

        [[nodiscard]] virtual bool UsesTab() const { return false; }

        virtual void Draw(const WindowInput& a_input) = 0;
    };
}
