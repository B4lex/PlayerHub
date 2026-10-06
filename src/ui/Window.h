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

        [[nodiscard]] virtual bool IsReleaseKey(std::uint32_t) const { return false; }

        [[nodiscard]] virtual bool UsesMouse() const { return false; }

        [[nodiscard]] virtual bool HidesIdleCursor() const { return false; }

        [[nodiscard]] virtual std::optional<ScreenPoint> GetCursorStart() const { return std::nullopt; }

        [[nodiscard]] virtual bool UsesTab() const { return false; }

        virtual void Draw(const WindowInput& a_input) = 0;
    };
}
