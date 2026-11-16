#include "ui/widgets/VoiceGlyphs.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace UI::VoiceGlyphs
{
    namespace
    {
        // Each bar's share of the level, so they rise unevenly, as an equalizer's do.
        constexpr std::array<float, 3> kBarWeights{ 0.6F, 1.0F, 0.75F };
        constexpr float kRestingHeight = 0.25F;
    }

    void DrawSpeaking(ImDrawList& a_drawList, ImVec2 a_min, float a_size, ImU32 a_color, float a_level)
    {
        const auto gap = std::floor(a_size / 7.0F);
        const auto width = std::floor((a_size - gap * 2.0F) / 3.0F);
        const auto bottom = a_min.y + a_size;
        const auto level = std::clamp(a_level, 0.0F, 1.0F);
        for (std::size_t bar = 0; bar < kBarWeights.size(); ++bar)
        {
            const auto height = std::round(a_size * std::lerp(kRestingHeight, 1.0F, level * kBarWeights[bar]));
            const auto left = a_min.x + static_cast<float>(bar) * (width + gap);
            a_drawList.AddRectFilled(ImVec2{ left, bottom - height }, ImVec2{ left + width, bottom }, a_color,
                width * 0.5F);
        }
    }
}
