#pragma once

#include <imgui.h>

#include <optional>

namespace UI::TradeDock
{
    // The trade layout's chat column.
    struct Column
    {
        ImVec2 min;
        ImVec2 size;
        // The trade layout's font size, after the overlay scale.
        float fontSize{ 0.0F };
    };

    // By the trade window, in each frame it draws its panel, before the chat draws.
    void Set(const Column& a_column);

    // The column set in this frame, if any.
    [[nodiscard]] std::optional<Column> Get();
}
