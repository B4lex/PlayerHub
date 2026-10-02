#pragma once

#include <imgui.h>

namespace UI::MenuFrame
{
    // Skyrim's menu frame, lines with a knot in each corner, around the
    // plugin's interactive windows. Its shape is a file shipped beside the
    // plugin; without it a window has the rounded rectangle of its style.

    // Reads the shape. Once, before the first Draw.
    void Load();

    // In pixels at the current font size.
    struct Metrics
    {
        // From the window's edge to the inside of the frame's lines.
        float inset{ 0.0F };
        // From a window corner, along both of its edges, to where its knot
        // ends. Content outside that square is clear of the knot.
        float corner{ 0.0F };
        // Of the background's corners.
        float rounding{ 0.0F };
    };
    [[nodiscard]] Metrics GetMetrics();

    // The current window's background in a_backing and its frame in a_line,
    // for a window begun with ImGuiWindowFlags_NoBackground. a_line is
    // opaque: the anti-aliased fill of pieces this fine overlaps its own
    // triangles, which a translucent color shows as brighter specks.
    // Overlay thread, before the window's content.
    void Draw(ImU32 a_line, ImU32 a_backing);
}
