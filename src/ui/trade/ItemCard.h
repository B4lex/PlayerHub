#pragma once

#include <cstdint>
#include <string_view>

namespace Forms
{
    struct ItemDetails;
}

namespace UI
{
    struct ItemCard
    {
        std::string_view name;
        std::uint32_t count{ 0 };
        const Forms::ItemDetails* details{ nullptr };
        // Why the item cannot be offered, or empty.
        std::string_view blocked;
        std::uint32_t formID{ 0 };
    };

    // The height DrawItemCard takes at a_width, for sizing its window to it before Begin.
    [[nodiscard]] float MeasureItemCard(const ItemCard& a_card, float a_width);

    // Draws at the cursor across the content region.
    void DrawItemCard(const ItemCard& a_card);
}
