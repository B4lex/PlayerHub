#include "ui/Cursor.h"
#include "ui/Fonts.h"
#include "ui/InputCapture.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <numbers>
#include <vector>

namespace UI::Cursor
{
    namespace
    {
        constexpr float kUnitFonts = 0.9F / 16.0F;
        constexpr float kLineUnits = 2.0F;
        constexpr float kBorderUnits = 0.95F;
        constexpr ImVec4 kBacking{ 0.03F, 0.03F, 0.03F, 1.0F };
        constexpr float kVanillaHeightFonts = 2.0F;
        // The woven pointer's reach right of and below its hot spot.
        constexpr float kPointerExtentUnits = 33.0F;
        constexpr float kTooltipGapFonts = 0.25F;
        // Dear ImGui keeps a tooltip out of the box from the mouse to 24 px times
        // MouseCursorScale right and down, beside it when it fits and else below it
        // (FindBestWindowPosForTooltip in 1.92.9), for a cursor of its own size.
        constexpr float kImGuiTooltipAvoid = 24.0F;
        constexpr ImU32 kVanillaOutline = IM_COL32(0, 0, 0, 204);
        constexpr ImU32 kVanillaBand = IM_COL32(187, 189, 191, 153);
        constexpr ImU32 kVanillaCentre = IM_COL32(0, 0, 0, 255);
        // U+E005 to U+E007 of PlayerHub.ttf.
        constexpr const char* kVanillaOutlineGlyph = "\xEE\x80\x85";
        constexpr const char* kVanillaBandGlyph = "\xEE\x80\x86";
        constexpr const char* kVanillaCentreGlyph = "\xEE\x80\x87";

        struct Strand
        {
            std::vector<ImVec2> points;
            bool closed{ false };
            // Convex closed strands only.
            bool core{ false };
            // A piece of a strand drawn again over what crossed it. Its border stops a point
            // short of each end, so the border's ends lie under the line instead of across it.
            bool over{ false };
        };

        // A layer draws every core, then every border, then every line, so its strands join
        // without a border between them, while a later layer's border cuts the lines it crosses.
        using Layer = std::vector<Strand>;

        struct Shape
        {
            std::vector<Layer> layers;
            ImVec2 hotspot{};
        };

        struct Shapes
        {
            Shape pointer;
            Shape text;
            Shape move;
            Shape resizeEw;
            Shape resizeNs;
            Shape resizeNesw;
            Shape resizeNwse;
        };

        ImVec2 Lerp(ImVec2 a_from, ImVec2 a_to, float a_t)
        {
            return ImVec2{ a_from.x + (a_to.x - a_from.x) * a_t, a_from.y + (a_to.y - a_from.y) * a_t };
        }

        float Distance(ImVec2 a_first, ImVec2 a_second)
        {
            return std::hypot(a_first.x - a_second.x, a_first.y - a_second.y);
        }

        // Appends the curve's points after a_from, up to and including a_to.
        void AppendQuad(std::vector<ImVec2>& a_points, ImVec2 a_from, ImVec2 a_control, ImVec2 a_to, int a_segments)
        {
            for (int i = 1; i <= a_segments; ++i)
            {
                const auto t = static_cast<float>(i) / static_cast<float>(a_segments);
                a_points.push_back(Lerp(Lerp(a_from, a_control, t), Lerp(a_control, a_to, t), t));
            }
        }

        std::vector<ImVec2> Ring(ImVec2 a_center, float a_radius, int a_segments)
        {
            std::vector<ImVec2> points;
            points.reserve(static_cast<std::size_t>(a_segments));
            for (int i = 0; i < a_segments; ++i)
            {
                const auto angle = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(a_segments);
                points.push_back(ImVec2{ a_center.x + a_radius * std::cos(angle), a_center.y + a_radius * std::sin(angle) });
            }
            return points;
        }

        std::vector<ImVec2> Rotated(std::vector<ImVec2> a_points, float a_degrees)
        {
            const auto radians = a_degrees * std::numbers::pi_v<float> / 180.0F;
            const auto cos = std::cos(radians);
            const auto sin = std::sin(radians);
            for (auto& point : a_points)
            {
                point = ImVec2{ point.x * cos - point.y * sin, point.x * sin + point.y * cos };
            }
            return a_points;
        }

        // ImGui joins thick lines with full miters, so a line's outer corner at a_corner lies
        // along the bisector at half the thickness over the sine of half the angle.
        ImVec2 OuterCorner(ImVec2 a_previous, ImVec2 a_corner, ImVec2 a_next, float a_halfThickness)
        {
            const auto unit = [&](ImVec2 a_to) {
                const auto length = Distance(a_to, a_corner);
                return ImVec2{ (a_to.x - a_corner.x) / length, (a_to.y - a_corner.y) / length };
            };
            const auto first = unit(a_previous);
            const auto second = unit(a_next);
            const ImVec2 sum{ first.x + second.x, first.y + second.y };
            const auto sumLength = std::hypot(sum.x, sum.y);
            const auto halfAngleSine = Distance(first, second) * 0.5F;
            const auto reach = a_halfThickness / halfAngleSine;
            return ImVec2{ a_corner.x - sum.x / sumLength * reach, a_corner.y - sum.y / sumLength * reach };
        }

        Shape MakePointer()
        {
            constexpr ImVec2 kTip{ 0.0F, 0.0F };
            constexpr ImVec2 kWing{ 28.5F, 11.0F };
            constexpr ImVec2 kHeel{ 12.0F, 27.0F };
            constexpr ImVec2 kBackControl{ 22.5F, 22.5F };
            constexpr ImVec2 kRingCenter{ 21.6F, 21.4F };
            constexpr float kRingRadius = 4.4F;
            // Where the ring crosses the back on the heel's side, and how much of the back
            // around it passes over the ring.
            constexpr ImVec2 kOverCrossing{ 18.2F, 24.4F };
            constexpr float kOverReach = 3.0F;

            Strand body{ .points = { kTip, kWing }, .closed = true, .core = true };
            AppendQuad(body.points, kWing, kBackControl, kHeel, 24);

            Strand over{ .over = true };
            for (const auto& point : body.points)
            {
                if (Distance(point, kOverCrossing) <= kOverReach)
                {
                    over.points.push_back(point);
                }
            }

            Shape shape;
            shape.hotspot = OuterCorner(kHeel, kTip, kWing, kLineUnits * 0.5F);
            shape.layers.push_back(Layer{ body });
            shape.layers.push_back(Layer{ Strand{ .points = Ring(kRingCenter, kRingRadius, 40), .closed = true } });
            shape.layers.push_back(Layer{ over });
            return shape;
        }

        // Points along +x with its back bowed toward the centre, clockwise on screen.
        std::vector<ImVec2> ArrowHead()
        {
            constexpr ImVec2 kApex{ 19.0F, 0.0F };
            constexpr ImVec2 kBackLower{ 11.4F, 4.7F };
            constexpr ImVec2 kBackUpper{ 11.4F, -4.7F };
            constexpr ImVec2 kBackControl{ 9.0F, 0.0F };

            std::vector<ImVec2> points{ kApex, kBackLower };
            AppendQuad(points, kBackLower, kBackControl, kBackUpper, 10);
            return points;
        }

        Shape MakeArrows(std::initializer_list<float> a_angles, float a_ringRadius)
        {
            Layer layer;
            for (const auto angle : a_angles)
            {
                layer.push_back(Strand{ .points = Rotated(ArrowHead(), angle), .closed = true, .core = true });
            }
            layer.push_back(Strand{ .points = Ring(ImVec2{}, a_ringRadius, 24), .closed = true, .core = true });

            Shape shape;
            shape.layers.push_back(layer);
            return shape;
        }

        Shape MakeText()
        {
            Layer layer;
            layer.push_back(Strand{ .points = { ImVec2{ 0.0F, -8.5F }, ImVec2{ 0.0F, 8.5F } } });
            layer.push_back(Strand{ .points = { ImVec2{ -3.4F, -10.2F }, ImVec2{ 0.0F, -8.5F }, ImVec2{ 3.4F, -10.2F } } });
            layer.push_back(Strand{ .points = { ImVec2{ -3.4F, 10.2F }, ImVec2{ 0.0F, 8.5F }, ImVec2{ 3.4F, 10.2F } } });

            Shape shape;
            shape.layers.push_back(layer);
            return shape;
        }

        const Shapes& GetShapes()
        {
            static const Shapes shapes{
                .pointer = MakePointer(),
                .text = MakeText(),
                .move = MakeArrows({ 0.0F, 90.0F, 180.0F, 270.0F }, 3.2F),
                .resizeEw = MakeArrows({ 0.0F, 180.0F }, 2.8F),
                .resizeNs = MakeArrows({ 90.0F, 270.0F }, 2.8F),
                .resizeNesw = MakeArrows({ 135.0F, 315.0F }, 2.8F),
                .resizeNwse = MakeArrows({ 45.0F, 225.0F }, 2.8F),
            };
            return shapes;
        }

        const Shape& GetShape(ImGuiMouseCursor a_cursor)
        {
            const auto& shapes = GetShapes();
            switch (a_cursor)
            {
            case ImGuiMouseCursor_TextInput:
                return shapes.text;
            case ImGuiMouseCursor_ResizeAll:
                return shapes.move;
            case ImGuiMouseCursor_ResizeEW:
                return shapes.resizeEw;
            case ImGuiMouseCursor_ResizeNS:
                return shapes.resizeNs;
            case ImGuiMouseCursor_ResizeNESW:
                return shapes.resizeNesw;
            case ImGuiMouseCursor_ResizeNWSE:
                return shapes.resizeNwse;
            default:
                return shapes.pointer;
            }
        }

        std::vector<ImVec2> s_points;

        void Place(const Strand& a_strand, ImVec2 a_origin, float a_unit, bool a_trimEnds)
        {
            const auto skip = a_trimEnds && a_strand.points.size() > 2 ? std::size_t{ 1 } : std::size_t{ 0 };
            s_points.clear();
            for (auto i = skip; i + skip < a_strand.points.size(); ++i)
            {
                const auto& point = a_strand.points[i];
                s_points.push_back(ImVec2{ a_origin.x + point.x * a_unit, a_origin.y + point.y * a_unit });
            }
        }
    }

    void PlaceTooltips(ImGuiStyle& a_style)
    {
        const auto fontSize = a_style.FontSizeBase * a_style.FontScaleMain * a_style.FontScaleDpi;
        // The game's cursor is a little narrower than it is tall.
        const auto extent = Fonts::GetLogo() ? kVanillaHeightFonts * fontSize : kPointerExtentUnits * kUnitFonts * fontSize;
        a_style.MouseCursorScale = (extent + kTooltipGapFonts * fontSize) / kImGuiTooltipAvoid;
    }

    void Draw()
    {
        if (!InputCapture::ShowsCursor())
        {
            return;
        }
        const auto cursor = ImGui::GetMouseCursor();
        if (cursor == ImGuiMouseCursor_None || !ImGui::IsMousePosValid())
        {
            return;
        }

        const auto& shape = GetShape(cursor);
        const auto& mouse = ImGui::GetIO().MousePos;
        auto& list = *ImGui::GetForegroundDrawList();
        auto* const vanilla = Fonts::GetLogo();
        if (&shape == &GetShapes().pointer && vanilla)
        {
            const auto height = kVanillaHeightFonts * ImGui::GetFontSize();
            list.AddText(vanilla, height, mouse, kVanillaOutline, kVanillaOutlineGlyph);
            list.AddText(vanilla, height, mouse, kVanillaBand, kVanillaBandGlyph);
            list.AddText(vanilla, height, mouse, kVanillaCentre, kVanillaCentreGlyph);
            return;
        }

        const auto unit = kUnitFonts * ImGui::GetFontSize();
        const ImVec2 origin{ mouse.x - shape.hotspot.x * unit, mouse.y - shape.hotspot.y * unit };
        const auto line = ImGui::GetColorU32(Theme::GetFrameColor());
        const auto backing = ImGui::GetColorU32(kBacking);

        for (const auto& layer : shape.layers)
        {
            for (const auto& strand : layer)
            {
                if (strand.core)
                {
                    Place(strand, origin, unit, false);
                    list.AddConvexPolyFilled(s_points.data(), static_cast<int>(s_points.size()), backing);
                }
            }
            for (const auto pass : { 0, 1 })
            {
                const bool border = pass == 0;
                const auto thickness = (border ? kLineUnits + 2.0F * kBorderUnits : kLineUnits) * unit;
                for (const auto& strand : layer)
                {
                    Place(strand, origin, unit, border && strand.over);
                    list.AddPolyline(s_points.data(), static_cast<int>(s_points.size()), border ? backing : line,
                        strand.closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
                }
            }
        }
    }
}
