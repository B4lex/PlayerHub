#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI
{
    // How much history the open chat shows, which [UI] ChatWidth and
    // ChatLines keep. A drag on the open chat's top edge, its right edge, or
    // the corner between them moves it with the cursor; when the drag ends
    // it settles on whole font sizes and lines, and those are saved.
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

        // What a size is measured in this frame.
        struct Units
        {
            // What one font size of width and one line take, in pixels.
            ImVec2 step;
            // What the screen has room for.
            Extent maximum;
        };

        // Where the current window takes a drag, in pixels.
        struct Grip
        {
            // In from the window's right and top edges.
            ImVec2 depth;
            // Along both of them from their corner: where a drag moves both.
            float corner{ 0.0F };
        };

        // Of the lines, in pixels: the configured size, or where a drag has
        // it; no more than the screen has room for and no less than the
        // smallest chat.
        [[nodiscard]] ImVec2 Get(const Units& a_units) const;

        // Moves the size with the cursor during a drag, and ends the drag
        // when the button is released. Each frame the chat is open, before
        // its window is begun, so the window has the size the cursor is at.
        void Follow(const Units& a_units);

        // Starts a drag on a press over a grip of the current window. Each
        // frame the chat is open, after Begin.
        void Grab(const Grip& a_grip, const Units& a_units);

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
        // During a drag, in pixels: the size when it started, and now.
        ImVec2 origin;
        ImVec2 size;
        // The whole size nearest to that, which the drag's end saves.
        Extent settled;
    };
}
