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

        void Follow(const Units& a_units);

        void Grab(const Grip& a_grip, const Units& a_units);

        void Release();

    private:
        enum Edge : std::uint8_t
        {
            kNone = 0,
            kTop = 1 << 0,
            kRight = 1 << 1
        };

        [[nodiscard]] static std::uint8_t FindEdges(const Grip& a_grip);
        static void ShowResizeCursor(std::uint8_t a_edges);

        std::uint8_t edges{ kNone };
        ImVec2 origin;
        ImVec2 size;
        Extent settled;
    };
}
