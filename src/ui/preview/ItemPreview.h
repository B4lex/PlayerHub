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

    // Draws the item's model at the cursor, fitted in a_size over whatever is behind it, and
    // turns it while the player drags it.
    void DrawModel(std::uint32_t a_formID, const ImVec2& a_size);

    // Fills the rectangle with a_background, which must be opaque since the still is captured on
    // it, and draws the item's still into it once captured.
    Thumbnail DrawThumbnail(ImDrawList& a_drawList, std::uint32_t a_formID, ImU32 a_background, const ImVec2& a_min,
        const ImVec2& a_max);

    void Update();
}
