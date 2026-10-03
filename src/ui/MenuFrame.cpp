#include "ui/MenuFrame.h"
#include "config/PluginConfig.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace UI::MenuFrame
{
    namespace
    {
        // The game's own art, so it is a file beside the plugin rather than
        // part of it. Relative to the plugin's directory.
        constexpr std::wstring_view kShapeFile = L"skyrim-mp/frame/menu-frame.shape";
        constexpr std::string_view kFormat = "frame 1";

        // A pixel of the art in font sizes at [UI] FrameThickness 100, so the
        // frame follows the overlay scale. The game lays its menus out for a
        // 720p screen, which makes the pixel 1.5 px at 1080p, where the base
        // font is 16 px: the frame is then as large as around the game's own
        // message boxes.
        constexpr float kArtPixelFonts = 1.5F / 16.0F;

        constexpr float kMaximumLength = 4096.0F;
        constexpr std::size_t kMaximumPoints = 8192;
        constexpr std::uint32_t kMinimumPolygonPoints = 3;

        struct Point
        {
            // In the art's pixels, from the window's left or right edge and
            // from its top or bottom one.
            ImVec2 offset;
            bool fromRight{ false };
            bool fromBottom{ false };
        };

        struct Polygon
        {
            std::uint32_t pointCount{ 0 };
            // The silhouette behind the lines rather than a part of them.
            bool backing{ false };
        };

        struct Shape
        {
            // A polygon's points follow those of the polygons before it.
            std::vector<Point> points;
            std::vector<Polygon> polygons;
            float inset{ 0.0F };
            float corner{ 0.0F };
        };

        // What decides the triangles of a window's frame apart from where
        // the window is.
        struct FrameKey
        {
            ImVec2 size;
            float scale{ 0.0F };
            ImU32 line{ 0 };
            ImU32 backing{ 0 };
            ImVec2 whitePixel;
            ImDrawListFlags flags{ ImDrawListFlags_None };
            float fringe{ 0.0F };

            bool operator==(const FrameKey& a_other) const
            {
                return size.x == a_other.size.x && size.y == a_other.size.y && scale == a_other.scale
                    && line == a_other.line && backing == a_other.backing && whitePixel.x == a_other.whitePixel.x
                    && whitePixel.y == a_other.whitePixel.y && flags == a_other.flags && fringe == a_other.fringe;
            }
        };

        // A frame as last triangulated, which is most of what drawing one
        // costs: vertices from the window's corner and indices from the first
        // vertex. A window drawn again with the same key copies them.
        struct DrawnFrame
        {
            FrameKey key;
            std::vector<ImDrawVert> vertices;
            std::vector<ImDrawIdx> indices;
            int usedFrame{ -1 };
        };

        // One per framed window, and one more for a window that comes and goes.
        constexpr std::size_t kDrawnFrames = 4;

        // Empty until Load reads a shape.
        Shape s_shape;
        std::array<DrawnFrame, kDrawnFrames> s_drawn;

        std::string_view NextWord(std::string_view& a_line)
        {
            const auto start = a_line.find_first_not_of(" \t");
            if (start == std::string_view::npos)
            {
                a_line = {};
                return {};
            }
            const auto end = a_line.find_first_of(" \t", start);
            const auto word = a_line.substr(start, end == std::string_view::npos ? end : end - start);
            a_line.remove_prefix(end == std::string_view::npos ? a_line.size() : end);
            return word;
        }

        bool ParseLength(std::string_view a_word, float& a_value)
        {
            const auto [end, error] = std::from_chars(a_word.data(), a_word.data() + a_word.size(), a_value);
            return error == std::errc{} && end == a_word.data() + a_word.size() && std::isfinite(a_value)
                && a_value >= 0.0F && a_value <= kMaximumLength;
        }

        bool ParseCount(std::string_view a_word, std::uint32_t& a_value)
        {
            const auto [end, error] = std::from_chars(a_word.data(), a_word.data() + a_word.size(), a_value);
            return error == std::errc{} && end == a_word.data() + a_word.size();
        }

        bool ParsePoint(std::string_view a_sides, std::string_view a_line, Point& a_point)
        {
            if (a_sides.size() != 2 || (a_sides[0] != 'l' && a_sides[0] != 'r')
                || (a_sides[1] != 't' && a_sides[1] != 'b'))
            {
                return false;
            }
            a_point.fromRight = a_sides[0] == 'r';
            a_point.fromBottom = a_sides[1] == 'b';
            return ParseLength(NextWord(a_line), a_point.offset.x) && ParseLength(NextWord(a_line), a_point.offset.y)
                && NextWord(a_line).empty();
        }

        // Null, or what is wrong with a_text.
        const char* Parse(std::string_view a_text, Shape& a_shape)
        {
            bool formatSeen = false;
            std::uint32_t pointsLeft = 0;
            while (!a_text.empty())
            {
                const auto lineEnd = a_text.find('\n');
                auto line = a_text.substr(0, lineEnd);
                a_text.remove_prefix(lineEnd == std::string_view::npos ? a_text.size() : lineEnd + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                if (!formatSeen)
                {
                    if (line != kFormat)
                    {
                        return "not a menu frame of this version";
                    }
                    formatSeen = true;
                    continue;
                }

                const auto word = NextWord(line);
                if (pointsLeft > 0)
                {
                    Point point;
                    if (!ParsePoint(word, line, point))
                    {
                        return "a point is malformed";
                    }
                    a_shape.points.push_back(point);
                    --pointsLeft;
                    continue;
                }

                const auto value = NextWord(line);
                if (!NextWord(line).empty())
                {
                    return "a line has too many words";
                }
                if (word == "inset" || word == "corner")
                {
                    if (!ParseLength(value, word == "inset" ? a_shape.inset : a_shape.corner))
                    {
                        return "a length is malformed";
                    }
                }
                else if (word == "backing" || word == "line")
                {
                    if (!ParseCount(value, pointsLeft) || pointsLeft < kMinimumPolygonPoints
                        || pointsLeft > kMaximumPoints - a_shape.points.size())
                    {
                        return "a polygon's point count is out of range";
                    }
                    a_shape.polygons.push_back(Polygon{ .pointCount = pointsLeft, .backing = word == "backing" });
                }
                else
                {
                    return "a line is not understood";
                }
            }
            if (pointsLeft > 0)
            {
                return "the last polygon is cut short";
            }
            if (a_shape.polygons.empty())
            {
                return "it has no polygons";
            }
            if (a_shape.corner < a_shape.inset)
            {
                return "the corner is smaller than the inset";
            }
            return nullptr;
        }

        // Screen pixels per pixel of the art.
        float GetScale()
        {
            return ImGui::GetFontSize() * kArtPixelFonts * static_cast<float>(Config::Get()->ui.frameThickness) * 0.01F;
        }

        void Copy(const DrawnFrame& a_frame, ImDrawList& a_list, const ImVec2& a_min)
        {
            a_list.PrimReserve(static_cast<int>(a_frame.indices.size()), static_cast<int>(a_frame.vertices.size()));
            // After the reserve, which may start the vertices over.
            const auto first = a_list._VtxCurrentIdx;
            for (const auto index : a_frame.indices)
            {
                a_list.PrimWriteIdx(static_cast<ImDrawIdx>(first + index));
            }
            for (const auto& vertex : a_frame.vertices)
            {
                a_list.PrimWriteVtx(ImVec2{ a_min.x + vertex.pos.x, a_min.y + vertex.pos.y }, vertex.uv, vertex.col);
            }
        }

        // Draws the frame and keeps its triangles in a_frame for the next
        // frames, unless the list started its vertices over meanwhile.
        void Triangulate(DrawnFrame& a_frame, ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_max)
        {
            const auto firstVertex = a_list.VtxBuffer.Size;
            const auto firstIndex = a_list.IdxBuffer.Size;
            const auto first = a_list._VtxCurrentIdx;
            const auto vertexOffset = a_list._CmdHeader.VtxOffset;

            const auto scale = a_frame.key.scale;
            const auto* point = s_shape.points.data();
            for (const auto& polygon : s_shape.polygons)
            {
                for (auto left = polygon.pointCount; left > 0; --left, ++point)
                {
                    const auto x = point->offset.x * scale;
                    const auto y = point->offset.y * scale;
                    a_list.PathLineTo(ImVec2{ point->fromRight ? a_max.x - x : a_min.x + x,
                        point->fromBottom ? a_max.y - y : a_min.y + y });
                }
                a_list.PathFillConcave(polygon.backing ? a_frame.key.backing : a_frame.key.line);
            }

            if (a_list._CmdHeader.VtxOffset != vertexOffset)
            {
                a_frame.key = FrameKey{};
                return;
            }
            a_frame.vertices.assign(a_list.VtxBuffer.Data + firstVertex, a_list.VtxBuffer.Data + a_list.VtxBuffer.Size);
            for (auto& vertex : a_frame.vertices)
            {
                vertex.pos = ImVec2{ vertex.pos.x - a_min.x, vertex.pos.y - a_min.y };
            }
            a_frame.indices.assign(a_list.IdxBuffer.Data + firstIndex, a_list.IdxBuffer.Data + a_list.IdxBuffer.Size);
            for (auto& index : a_frame.indices)
            {
                index = static_cast<ImDrawIdx>(index - first);
            }
        }
    }

    void Load()
    {
        const auto directory = Config::GetDirectory();
        if (directory.empty())
        {
            return;
        }
        const auto path = directory / kShapeFile;
        std::ifstream input{ path, std::ios::binary };
        if (!input)
        {
            logs::warn("UI overlay: could not read menu frame {}; windows have a plain border", path.string());
            return;
        }
        const std::string text{ std::istreambuf_iterator<char>{ input }, std::istreambuf_iterator<char>{} };

        Shape shape;
        if (const auto* problem = Parse(text, shape))
        {
            logs::warn(
                "UI overlay: could not load menu frame {}: {}; windows have a plain border", path.string(), problem);
            return;
        }
        logs::info("UI overlay: menu frame {} with {} polygons, {} points", path.filename().string(),
            shape.polygons.size(), shape.points.size());
        s_shape = std::move(shape);
    }

    Metrics GetMetrics()
    {
        if (s_shape.polygons.empty())
        {
            const auto& style = ImGui::GetStyle();
            return Metrics{
                .inset = style.WindowBorderSize, .corner = style.WindowBorderSize, .rounding = style.WindowRounding
            };
        }
        const auto scale = GetScale();
        return Metrics{ .inset = std::ceil(s_shape.inset * scale), .corner = std::ceil(s_shape.corner * scale) };
    }

    void Draw(ImU32 a_line, ImU32 a_backing)
    {
        const auto min = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        const ImVec2 max{ min.x + size.x, min.y + size.y };
        auto& list = *ImGui::GetWindowDrawList();
        // A window's own clip rectangle leaves out part of its padding.
        list.PushClipRect(min, max);
        if (s_shape.polygons.empty())
        {
            const auto& style = ImGui::GetStyle();
            list.AddRectFilled(min, max, a_backing, style.WindowRounding);
            if (style.WindowBorderSize > 0.0F)
            {
                list.AddRect(min, max, a_line, style.WindowRounding, ImDrawFlags_None, style.WindowBorderSize);
            }
        }
        else
        {
            const FrameKey key{ .size = size,
                .scale = GetScale(),
                .line = a_line,
                .backing = a_backing,
                .whitePixel = ImGui::GetFontTexUvWhitePixel(),
                .flags = list.Flags,
                .fringe = list._FringeScale };
            auto drawn = std::ranges::find(s_drawn, key, &DrawnFrame::key);
            if (drawn != s_drawn.end())
            {
                Copy(*drawn, list, min);
            }
            else
            {
                drawn = std::ranges::min_element(s_drawn, {}, &DrawnFrame::usedFrame);
                drawn->key = key;
                Triangulate(*drawn, list, min, max);
            }
            drawn->usedFrame = ImGui::GetFrameCount();
        }
        list.PopClipRect();
    }
}
