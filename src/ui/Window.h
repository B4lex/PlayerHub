#pragma once

#include <cstdint>
#include <optional>

namespace UI
{
    struct WindowInput
    {
        // Whether the window holds input focus this frame. While a window does,
        // Dear ImGui receives the keyboard and the game no keyboard or mouse input.
        bool focused{ false };
        // Mouse wheel notches since the previous frame, positive away from the
        // user. Always zero without focus, and for a window that uses the mouse,
        // which scrolls through Dear ImGui instead.
        float wheelSteps{ 0.0F };
    };

    // A point on the screen, in back-buffer pixels.
    struct ScreenPoint
    {
        float x{ 0.0F };
        float y{ 0.0F };
    };

    // An independent overlay window, registered with WindowManager. Windows never
    // draw or reference each other; at most one holds input focus at a time.
    class Window
    {
    public:
        virtual ~Window() = default;

        // Whether pressing the DirectInput scan code during gameplay, with no
        // window focused, gives this window focus. The game never sees that press.
        [[nodiscard]] virtual bool IsFocusKey(std::uint32_t) const { return false; }

        // Whether pressing the scan code while this window holds focus releases
        // it. Neither the window nor the game sees that press. A key that edits
        // text goes to an active text field instead.
        [[nodiscard]] virtual bool IsReleaseKey(std::uint32_t) const { return false; }

        // Whether Dear ImGui receives the mouse, with a drawn cursor, while this
        // window holds focus. Otherwise the window is keyboard-only.
        [[nodiscard]] virtual bool UsesMouse() const { return false; }

        // For a window that uses the mouse: whether the cursor stays hidden, and
        // Dear ImGui without a mouse, until the mouse first moves after the
        // window took focus. Until then the wheel arrives as
        // WindowInput::wheelSteps.
        [[nodiscard]] virtual bool HidesIdleCursor() const { return false; }

        // Where the cursor appears; the middle of the screen when empty.
        [[nodiscard]] virtual std::optional<ScreenPoint> GetCursorStart() const { return std::nullopt; }

        // Whether Dear ImGui receives Tab while this window holds focus.
        // Otherwise Tab is dropped, so it cannot move keyboard focus off a text
        // field.
        [[nodiscard]] virtual bool UsesTab() const { return false; }

        // Between ImGui::NewFrame and ImGui::Render. A focused window gives focus
        // back with WindowManager::ReleaseFocus; it also loses focus whenever a
        // menu takes over input.
        virtual void Draw(const WindowInput& a_input) = 0;
    };
}
