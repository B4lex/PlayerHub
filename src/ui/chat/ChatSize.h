#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI
{
    class ChatSize
    {
    public:
        struct Extent
        {
            std::uint32_t width{ 0 };
            std::uint32_t lines{ 0 };

            bool operator==(const Extent&) const = default;
        };

        struct Units
        {
            ImVec2 step;
            Extent maximum;
        };

        struct Grip
        {
            ImVec2 depth;
            float corner{ 0.0F };
        };

        [[nodiscard]] ImVec2 Get(const Units& a_units) const;

        // The lower left corner of the frame: a_corner, or where a drag on the
        // left or bottom edge has moved it.
        [[nodiscard]] ImVec2 GetCorner(const ImVec2& a_corner) const;

        // The room for the frame from a_corner to the right and top screen edges,
        // or, on an axis whose opposite edge a drag holds in place, from that edge.
        [[nodiscard]] ImVec2 GetRoom(const ImVec2& a_corner) const;

        [[nodiscard]] bool IsGrowingLeft() const { return (edges & kLeft) != 0; }

        // a_around is the frame's size around the lines.
        void Follow(const Units& a_units, const ImVec2& a_around);

        // Whether a drag is held or the cursor is on a grip. a_corner is the
        // frame's configured lower left corner.
        bool Grab(const Grip& a_grip, const Units& a_units, const ImVec2& a_corner);

        void Release();

    private:
        enum Edge : std::uint8_t
        {
            kNone = 0,
            kTop = 1 << 0,
            kRight = 1 << 1,
            kBottom = 1 << 2,
            kLeft = 1 << 3
        };

        [[nodiscard]] static std::uint8_t FindEdges(const Grip& a_grip);
        // The lower left corner of a frame around lines of a_size.
        [[nodiscard]] ImVec2 Place(const ImVec2& a_size) const;
        static void ShowResizeCursor(std::uint8_t a_edges);

        std::uint8_t edges{ kNone };
        ImVec2 origin;
        ImVec2 size;
        Extent settled;
        ImVec2 step;
        // The frame's size around the lines.
        ImVec2 around;
        // The frame's top right corner when grabbed.
        ImVec2 fixed;
        ImVec2 corner;
    };
}
