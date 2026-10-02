#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI
{
    // How much history the open chat shows, which [UI] ChatWidth and
    // ChatLines keep. A drag on the open chat's top edge, its right edge, or
    // the corner between them changes it and saves it when it ends.
    class ChatSize
    {
    public:
        struct Extent
        {
            // In font sizes.
            std::uint32_t width{ 0 };
            std::uint32_t lines{ 0 };

            bool operator==(const Extent&) const = default;
        };

        // Where the current window takes a drag, in pixels.
        struct Grip
        {
            // In from the window's right and top edges.
            ImVec2 depth;
            // Along both of them from their corner: where a drag moves both.
            float corner{ 0.0F };
            // What one font size of width and one line take.
            ImVec2 step;
            // What the screen has room for.
            Extent maximum;
        };

        // The configured size, or what a drag has reached; no more than
        // a_maximum and no less than the smallest chat.
        [[nodiscard]] Extent Get(const Extent& a_maximum) const;

        // Starts, follows, and ends a drag in the current window. Each frame
        // the chat is open, after Begin.
        void Drag(const Grip& a_grip);

        // Ends a drag the mouse still holds, as when the chat loses focus.
        void Release();

    private:
        enum Edge : std::uint8_t
        {
            kNone = 0,
            kTop = 1 << 0,
            kRight = 1 << 1
        };

        // What a press under the cursor would drag.
        [[nodiscard]] static std::uint8_t FindEdges(const Grip& a_grip);
        static void ShowResizeCursor(std::uint8_t a_edges);

        // What a drag moves; kNone without one.
        std::uint8_t edges{ kNone };
        // During a drag: the size when it started, and now.
        Extent origin;
        Extent size;
    };
}
