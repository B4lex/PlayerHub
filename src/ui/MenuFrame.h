#pragma once

#include <imgui.h>

namespace UI::MenuFrame
{
    void Load();

    struct Metrics
    {
        float inset{ 0.0F };
        float corner{ 0.0F };
        float rounding{ 0.0F };
    };
    [[nodiscard]] Metrics GetMetrics();

    // The anti-aliased fill of pieces this fine overlaps its own triangles, which
    // a translucent line color shows as brighter specks.
    void Draw(ImU32 a_line, ImU32 a_backing);
}
