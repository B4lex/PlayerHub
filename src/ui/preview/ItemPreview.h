#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI::ItemPreview
{
    enum class Thumbnail : std::uint8_t
    {
        kDrawn,
        kLoading,
        kUnavailable
    };

    void Draw(std::uint32_t a_formID, ImVec2 a_size);

    // Fills the rectangle with a_background, which must be opaque since the still is captured on
    // it, and draws the item's still into it once captured.
    Thumbnail DrawThumbnail(
        ImDrawList& a_drawList, std::uint32_t a_formID, ImU32 a_background, const ImVec2& a_min, const ImVec2& a_max);

    void Update();
}
